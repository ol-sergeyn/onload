/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* X-SPDX-Copyright-Text: (c) Copyright 2023 Xilinx, Inc. */

/* Functions under test */
#include <ci/internal/ip_timestamp.h>

/* Test infrastructure */
#include "unit_test.h"
#include <ci/net/ipv4.h>

/* Helpers to build packets with trailers */
union pkt_buf {
  ci_ip_pkt_fmt pkt;
  uint8_t bytes[2048];
};

static void pkt_init(ci_ip_pkt_fmt* pkt)
{
  pkt->frag_next = OO_PP_ID_NULL;
  pkt->pkt_eth_payload_off = ETH_HLEN;
}

static void pkt_append(ci_ip_pkt_fmt* pkt, const void* data, int len)
{
  assert(pkt->pkt_start_off + pkt->pay_len + len <= sizeof(union pkt_buf));
  memcpy(PKT_START(pkt) + pkt->pay_len, data, len);
  pkt->pay_len += len;
}

static void pkt_udp(ci_ip_pkt_fmt* pkt, const void* data, int len)
{
  struct oo_eth_hdr eth = {0};
  ci_ip4_hdr ip4 = {0};
  ci_udp_hdr udp = {0};

  eth.ether_type = 0x0008;
  ip4.ip_ihl_version = 0x45;
  ip4.ip_tot_len_be16 = htons(sizeof(ip4) + sizeof(udp) + len);
  ip4.ip_protocol = 0x11;
  udp.udp_len_be16 = htons(sizeof(udp) + len);

  pkt_append(pkt, &eth, sizeof(eth));
  pkt_append(pkt, &ip4, sizeof(ip4));
  pkt_append(pkt, &udp, sizeof(udp));
  pkt_append(pkt, data, len);
}

static void pkt_fcs(ci_ip_pkt_fmt* pkt)
{
  if( pkt->unused_padding[0] == 0 ) {
    uint8_t data[4] = {0};
    pkt_append(pkt, data, 4);
  }
}

static void pkt_cpacket(ci_ip_pkt_fmt* pkt, uint32_t sec, uint32_t nsec)
{
  uint8_t data[12] = {
    sec >> 24, sec >> 16, sec >> 8, sec,
    nsec >> 24, nsec >> 16, nsec >> 8, nsec,
    pkt->unused_padding[0] ? 2 : 0, 0, 0, 0
  };

  pkt_fcs(pkt);
  pkt_append(pkt, data, 12);
}

static void pkt_tag_header(ci_ip_pkt_fmt* pkt, int type, int len)
{
  assert(type < (1 << 5));
  assert(len < 0x04);

  /* Abuse this unused byte to record the presence of tags */
  uint8_t last = pkt->unused_padding[0] ? 0 : 0x20;
  pkt->unused_padding[0] = 1;

  uint8_t header = type | last | (len << 6);
  pkt_append(pkt, &header, 1);
}

static void pkt_primary(ci_ip_pkt_fmt* pkt, int type, int value)
{
  assert(value < (1 << 24));

  uint8_t data[3] = {value >> 16, value >> 8, value};

  pkt_fcs(pkt);
  pkt_append(pkt, data, 3);
  pkt_tag_header(pkt, type, 0);
}

static void pkt_primary_ex(
    ci_ip_pkt_fmt* pkt, int type, const void* data, int len)
{
  assert(len % 4 == 3);

  pkt_fcs(pkt);
  pkt_append(pkt, data, len);
  pkt_tag_header(pkt, type, len / 4);
}

static void pkt_subnano(ci_ip_pkt_fmt* pkt, int value)
{
  pkt_primary(pkt, 1, value);
}

static void pkt_secondary(
    ci_ip_pkt_fmt* pkt, int type, const void* data, int len)
{
  assert(type < (1 << 16));
  assert(len % 4 == 0);
  assert(len >= 4);

  pkt_fcs(pkt);
  pkt_append(pkt, data, len);

  len = len / 4 - 1;
  assert(len < (1 << 10));
  uint8_t header[3] = {type >> 8, type, len >> 2};
  pkt_append(pkt, header, 3);
  pkt_tag_header(pkt, 0x1f, len & 3);
}

/* Test with no cpacket trailer */
static void test_rx_pkt_timestamp_cpacket_none(void)
{
  ci_ip_pkt_fmt* pkt;

  STATE_ALLOC(union pkt_buf, buf);
  STATE_ALLOC(struct onload_timestamp, ts);

  pkt = &buf->pkt;
  pkt_init(pkt);
  pkt_udp(pkt, NULL, 0);
  STATE_STASH(buf);

  ci_rx_pkt_timestamp_cpacket(pkt, ts);
  STATE_CHECK(ts, sec, 0);

  STATE_FREE(buf);
  STATE_FREE(ts);
}

/* Test with a cpacket trailer and no extensions */
static void test_rx_pkt_timestamp_cpacket_basic(void)
{
  ci_ip_pkt_fmt* pkt;
  uint32_t sec = 0x12345678;
  uint32_t nsec = 0xfedcba98;

  STATE_ALLOC(union pkt_buf, buf);
  STATE_ALLOC(struct onload_timestamp, ts);

  pkt = &buf->pkt;
  pkt_init(pkt);
  pkt_udp(pkt, NULL, 0);
  pkt_cpacket(pkt, sec, nsec);
  STATE_STASH(buf);

  ci_rx_pkt_timestamp_cpacket(pkt, ts);
  STATE_CHECK(ts, sec, sec);
  STATE_CHECK(ts, nsec, nsec);

  STATE_FREE(buf);
  STATE_FREE(ts);
}

/* Test with a cpacket trailer and a sub-nanosecond extension */
static void test_rx_pkt_timestamp_cpacket_subnano(void)
{
  ci_ip_pkt_fmt* pkt;
  uint32_t sec = 0x12345678;
  uint32_t nsec = 0xfedcba98;
  uint32_t subnano = 0x564738;

  STATE_ALLOC(union pkt_buf, buf);
  STATE_ALLOC(struct onload_timestamp, ts);

  pkt = &buf->pkt;
  pkt_init(pkt);
  pkt_udp(pkt, NULL, 0);
  pkt_subnano(pkt, subnano);
  pkt_cpacket(pkt, sec, nsec);
  STATE_STASH(buf);

  ci_rx_pkt_timestamp_cpacket(pkt, ts);
  STATE_CHECK(ts, sec, sec);
  STATE_CHECK(ts, nsec, nsec);
  STATE_CHECK(ts, nsec_frac, subnano);

  STATE_FREE(buf);
  STATE_FREE(ts);
}

/* Test with a cpacket trailer and many extensions */
static void test_rx_pkt_timestamp_cpacket_many(void)
{
  ci_ip_pkt_fmt* pkt;
  uint32_t sec = 0x12345678;
  uint32_t nsec = 0xfedcba98;
  uint32_t subnano = 0x564738;
  uint8_t stuff[128];

  STATE_ALLOC(union pkt_buf, buf);
  STATE_ALLOC(struct onload_timestamp, ts);

  pkt = &buf->pkt;
  pkt_init(pkt);
  pkt_udp(pkt, NULL, 0);
  pkt_primary(pkt, 7, 42);
  pkt_subnano(pkt, ~subnano);
  pkt_primary_ex(pkt, 8, stuff, 11);
  pkt_secondary(pkt, 42, stuff, 32);
  pkt_subnano(pkt, subnano);
  pkt_secondary(pkt, 42, stuff, 128);
  pkt_primary_ex(pkt, 8, stuff, 15);
  pkt_primary(pkt, 7, 42);
  pkt_cpacket(pkt, sec, nsec);
  STATE_STASH(buf);

  ci_rx_pkt_timestamp_cpacket(pkt, ts);
  STATE_CHECK(ts, sec, sec);
  STATE_CHECK(ts, nsec, nsec);
  STATE_CHECK(ts, nsec_frac, subnano);

  STATE_FREE(buf);
  STATE_FREE(ts);
}

/* Test with a cpacket trailer and extensions with a corrupt length */
static void test_rx_pkt_timestamp_cpacket_bogus(void)
{
  ci_ip_pkt_fmt* pkt;
  uint32_t sec = 0x12345678;
  uint32_t nsec = 0xfedcba98;
  uint32_t subnano = 0x564738;
  uint8_t stuff[3];

  STATE_ALLOC(union pkt_buf, buf);
  STATE_ALLOC(struct onload_timestamp, ts);

  pkt = &buf->pkt;
  pkt_init(pkt);
  pkt_udp(pkt, NULL, 0);
  pkt_subnano(pkt, subnano);
  pkt_append(pkt, stuff, 3);
  pkt_tag_header(pkt, 4, 2);
  pkt_cpacket(pkt, sec, nsec);
  STATE_STASH(buf);

  ci_rx_pkt_timestamp_cpacket(pkt, ts);
  STATE_CHECK(ts, sec, sec);
  STATE_CHECK(ts, nsec, nsec);
  STATE_CHECK(ts, nsec_frac, 0); /* didn't find it, but no disastrous outcome */

  STATE_FREE(buf);
  STATE_FREE(ts);
}

/* Test cpacket timestamp using a packet captured from a Metamako Metawatch */
static void test_rx_pkt_timestamp_cpacket_pcap_metawatch(void)
{
  static const uint8_t pcap[] = {
    0x00,0x0f,0x53,0x4c,0xc8,0xc0,0x00,0x0f,0x53,0x43,0x07,0x70,0x08,0x00,0x45,0x00,
    0x05,0xdc,0xf2,0xb3,0x20,0x00,0x40,0x11,0x03,0x34,0xac,0x10,0x83,0x8a,0xac,0x10,
    0x83,0x7e,0xc8,0xc2,0x30,0x39,0x20,0x08,0x8a,0x43,0xa7,0x62,0xb9,0xd1,0x2a,0x5c,
    0x1c,0x29,0xc5,0xc0,0x8d,0x9b,0x65,0x39,0xd9,0xbc,0x60,0xe0,0x92,0x52,0x7e,0x50,
    0x98,0x85,0x7c,0x84,0x20,0xf5,0xd4,0x76,0xd7,0x82,0xb6,0x5a,0x28,0x67,0x01,0x74,
    0xf2,0x7d,0xa1,0x8c,0x27,0x22,0x7d,0xb9,0x75,0x95,0xfe,0x96,0x54,0x4b,0xab,0x5a,
    0xe3,0x82,0x93,0x7b,0xe8,0xa0,0x30,0x86,0x9c,0x52,0xab,0xb6,0xf3,0x7d,0x02,0x3b,
    0xb3,0x92,0x01,0x0c,0xcd,0xe2,0x81,0x13,0x34,0x63,0x58,0x72,0xb6,0x24,0xec,0x3b,
    0x07,0xf6,0x67,0x6e,0x34,0xd6,0x97,0x51,0xa1,0x25,0xb7,0x6a,0x47,0xf3,0xa9,0xd1,
    0x53,0xab,0x13,0x73,0x7d,0x4f,0x46,0x80,0xce,0x15,0x48,0x4e,0x86,0xf2,0xc3,0x6e,
    0x1e,0x51,0xec,0xc5,0xd8,0xbb,0xe2,0xf6,0x33,0xc2,0xa5,0x49,0xa9,0xaf,0x1e,0xe5,
    0x26,0xc7,0x7a,0xcc,0x4c,0x5a,0x17,0x54,0x97,0x7f,0x99,0xd5,0xb1,0x58,0x17,0x62,
    0x89,0x8c,0x07,0xdb,0x04,0xa4,0x7a,0xb6,0xcc,0xe1,0xd1,0xfa,0x62,0xec,0xff,0x9d,
    0x8b,0x75,0x63,0xe6,0x80,0x75,0xaf,0x14,0xd2,0x87,0x63,0xdf,0xda,0xfe,0x50,0x29,
    0xb1,0xa0,0xfc,0x97,0x66,0x3a,0x80,0xbc,0x25,0x5e,0xe5,0x71,0xae,0xce,0x30,0xb0,
    0x0d,0x9e,0x7e,0x32,0x71,0x7d,0xfb,0xc6,0xd0,0x5c,0x7d,0x10,0xaf,0x62,0xab,0xd4,
    0xd2,0xd1,0x86,0x3a,0x13,0x91,0xf3,0x68,0xcf,0x00,0x76,0x43,0xb8,0xeb,0xef,0xfd,
    0x72,0xd5,0xfc,0x15,0x9d,0x1e,0xae,0x2c,0x7a,0xa1,0x5d,0xdd,0x15,0x83,0x0c,0x28,
    0xba,0xe3,0x9b,0xae,0x82,0x6c,0x21,0xfc,0x8d,0x52,0x5d,0x73,0x64,0x76,0xd9,0x7a,
    0x83,0xb4,0x74,0x70,0xa5,0x5d,0xdd,0x2c,0xea,0x09,0xd7,0x3d,0x59,0x3c,0xcb,0x19,
    0x4a,0xcb,0x95,0xea,0x8a,0x6c,0x56,0xc8,0x17,0x04,0xf6,0x5c,0xfc,0xf0,0x47,0xac,
    0x7e,0x3c,0xe9,0xc6,0x95,0x1e,0x30,0xf9,0x68,0x96,0xda,0x3f,0x7d,0x46,0x8e,0x76,
    0xaf,0x25,0xfb,0x73,0xf3,0x47,0xdd,0x8c,0xbe,0xfd,0xa4,0x12,0x04,0xd2,0xf7,0xe0,
    0xd5,0x64,0xea,0x17,0xe2,0x93,0x74,0xe4,0xc0,0x7e,0x72,0x3d,0xf6,0xa5,0xa0,0x56,
    0xde,0xad,0xd5,0xb8,0x40,0xe3,0x84,0x0e,0x60,0x7c,0x6c,0x8f,0x94,0x44,0x1c,0xd1,
    0xe9,0xad,0xb7,0x5b,0x4c,0x81,0x16,0x42,0x22,0x71,0xba,0xee,0xc4,0x67,0x0d,0xdd,
    0x19,0x98,0xfe,0x10,0x6c,0xc3,0x9d,0x48,0xfe,0x49,0x4e,0xe1,0x8d,0x57,0x73,0x70,
    0x34,0x6f,0xf7,0xa3,0x81,0x9f,0xac,0x6f,0xca,0x46,0x4b,0x14,0x9e,0x6d,0xe5,0x12,
    0xb9,0xc5,0xdd,0xc3,0xf3,0x70,0xda,0xd7,0x42,0xe4,0x1c,0xda,0x0d,0xd4,0xf4,0x4b,
    0xa0,0x1a,0x68,0xfd,0x36,0x00,0x60,0x2e,0xf7,0x2c,0x81,0xba,0xa9,0x7d,0x6d,0xd7,
    0x80,0xba,0xc7,0xd9,0xc6,0xd3,0x0f,0xba,0xa2,0x44,0xf0,0x7f,0xe3,0x12,0xfc,0xd7,
    0xba,0xd3,0x2e,0x01,0x72,0x5e,0x9f,0x19,0x1b,0x72,0xf6,0xb8,0x0e,0x0b,0x0e,0x51,
    0xa1,0xb3,0x58,0x8a,0x27,0x2e,0x3e,0xcd,0x0a,0x1b,0xa5,0x66,0x4b,0xda,0x53,0x10,
    0xbc,0xb8,0xe2,0xea,0x82,0x0a,0x27,0x7d,0xb3,0xea,0xec,0x0c,0xe5,0x0a,0x73,0xa2,
    0x8b,0x91,0x12,0xbc,0xe9,0x56,0xbc,0x47,0xf2,0x9e,0x9e,0x0b,0xe2,0xce,0x89,0x3b,
    0x0b,0x1e,0x81,0x89,0x54,0xa8,0x71,0x37,0x26,0x07,0xc9,0x62,0xd7,0x38,0xef,0x56,
    0xa4,0x1f,0x04,0x2d,0x68,0x88,0x36,0x83,0x7f,0xd7,0x0a,0x39,0xba,0x3c,0xb9,0xd8,
    0x04,0x30,0x4f,0x2e,0x97,0x8a,0x4e,0xa4,0x51,0x61,0x1d,0x33,0x1b,0xc9,0x6f,0x7c,
    0x1b,0x80,0xd9,0x8d,0x29,0x67,0xf7,0xb0,0xb6,0x44,0xa5,0xec,0x4e,0x3f,0x08,0x0b,
    0x4c,0x15,0x38,0x30,0x46,0x3f,0xf1,0xd4,0x06,0xd4,0x96,0x96,0xaa,0xaa,0xd1,0x60,
    0x07,0x3e,0xc5,0x38,0x87,0x5c,0xd2,0x23,0x4a,0x3f,0xe1,0x18,0xd6,0xb8,0x9f,0x88,
    0x7a,0xe7,0x83,0xfa,0xdd,0x0c,0x6e,0x12,0xac,0x7f,0x47,0xf3,0xf1,0x26,0xc2,0x34,
    0xd6,0x50,0x03,0x9d,0x04,0x86,0x94,0xca,0x25,0x4e,0xb0,0x28,0xdd,0x10,0xa4,0x75,
    0x08,0x79,0xbd,0x2b,0xd8,0x0b,0xd7,0x68,0xa9,0xb4,0xb1,0x56,0xf8,0x2a,0x63,0x4e,
    0xbf,0x6d,0xdd,0x4a,0x75,0x56,0x69,0x97,0x73,0x86,0x4b,0x8b,0x68,0xf2,0x8e,0xb0,
    0x63,0xb6,0x55,0x9f,0xa3,0x81,0x8e,0x05,0xc6,0x5f,0x98,0x88,0x47,0x62,0xd5,0x33,
    0xe6,0xc6,0x1b,0xcf,0x9f,0x0f,0xe6,0x15,0x60,0xa3,0x70,0xf3,0xce,0x56,0x03,0x18,
    0xd4,0x06,0x74,0x0d,0xfa,0x9d,0x22,0xa4,0xdb,0x2c,0x06,0x1f,0x0c,0x89,0xcc,0x13,
    0x47,0x93,0xa0,0x40,0x32,0x84,0x59,0xf7,0x6c,0xb6,0xe2,0x80,0xee,0x09,0x80,0x4b,
    0x89,0x77,0x9d,0x72,0xd5,0xb4,0x2e,0x9e,0x93,0x59,0xbf,0x96,0xa8,0x87,0x47,0x06,
    0xca,0x62,0xea,0x4f,0x63,0x15,0x85,0x57,0x75,0x75,0x95,0xf7,0x78,0x59,0x2a,0xf5,
    0x1c,0x76,0x9e,0x2f,0xa3,0x13,0xc5,0xa6,0xc9,0x10,0xdc,0x60,0xba,0xb2,0x67,0xa0,
    0x2d,0x9f,0x7c,0xbb,0x73,0x0e,0x7c,0xd9,0xa9,0xa7,0x71,0xb1,0xdf,0x0d,0x93,0x53,
    0x95,0x6a,0x27,0x12,0x27,0xd1,0x22,0x01,0xba,0x9d,0x1c,0x63,0x9a,0x4e,0xa7,0xc3,
    0x2f,0xf7,0x1c,0x73,0xda,0x00,0x53,0x72,0x9e,0x33,0x2b,0xd0,0x74,0x9d,0x41,0x8d,
    0x4b,0xb7,0x32,0x1c,0x50,0x7b,0x47,0xc5,0xa1,0x0f,0x6b,0x71,0x81,0xac,0xf3,0x3b,
    0x6a,0x23,0x20,0x23,0x9f,0x82,0x28,0x4d,0x9a,0x8a,0x1e,0xec,0x70,0x5c,0x5a,0xf9,
    0xc2,0x62,0x76,0xcd,0x0b,0x35,0x19,0xb2,0x70,0x37,0xf8,0xb2,0xff,0x75,0x14,0x62,
    0xc6,0x2a,0x15,0x73,0x9d,0x2f,0x27,0x19,0x98,0xd6,0xe9,0x8f,0xe8,0x4d,0x26,0x18,
    0xb3,0xa0,0x03,0xf6,0xa7,0xcb,0x11,0xa2,0x0a,0x13,0x60,0xc8,0x72,0x48,0xf0,0x6e,
    0x88,0x5e,0x22,0xae,0x56,0x39,0x53,0x73,0x04,0xb1,0x03,0x44,0xab,0x12,0xaa,0x7d,
    0xe6,0xd7,0xe9,0x4d,0x1c,0x30,0x69,0x3b,0x6d,0xa9,0x87,0xca,0x6d,0xc4,0x38,0x88,
    0x93,0x32,0xe7,0xcf,0xfb,0xd8,0xbc,0xe2,0x4b,0xb4,0xc4,0x60,0x96,0x8c,0x70,0x25,
    0xc5,0xec,0xca,0x7c,0x04,0xb8,0x3f,0x80,0x75,0x70,0x1f,0xaa,0xaf,0x59,0x9b,0xe4,
    0x86,0x1a,0xfe,0x74,0x25,0xba,0x49,0xc0,0x60,0x9b,0x82,0x2d,0xe5,0x80,0xb7,0x78,
    0x0d,0x3a,0x3c,0xeb,0xce,0x31,0x30,0x11,0xb0,0x33,0x7d,0xa9,0x82,0x87,0x33,0x73,
    0x43,0xe9,0x20,0xa6,0x9d,0x34,0xad,0x5d,0x5f,0x1f,0x29,0xdf,0x4c,0xc0,0x36,0x0e,
    0xfc,0xf7,0x7e,0xc2,0x18,0x75,0xc7,0xe5,0x91,0x4f,0x19,0xfd,0x1e,0xf3,0x12,0x95,
    0xab,0x31,0x05,0x7a,0x6e,0xff,0x6c,0x18,0xb6,0xd1,0xc9,0x0b,0x38,0x43,0x4d,0x6b,
    0x87,0x83,0xe3,0xbc,0x1e,0xcd,0x17,0x14,0xc9,0xc3,0x7c,0x96,0x4e,0x3a,0x83,0x45,
    0xc3,0xdd,0x23,0x86,0x4f,0xe1,0x72,0x32,0x46,0x22,0x13,0x38,0xd5,0xb5,0x78,0xdc,
    0xa3,0x65,0x10,0x38,0x0d,0xb0,0xba,0x97,0x40,0xd9,0x54,0xab,0xf2,0x6c,0xca,0x42,
    0xd0,0x4b,0x93,0xe5,0x0c,0xf2,0x82,0x68,0xc3,0x6d,0x17,0x06,0x6c,0x6d,0xf6,0xa9,
    0xee,0x88,0x9a,0x2f,0x74,0x23,0x10,0x26,0xbc,0x4f,0x7d,0xe4,0x11,0x9a,0x86,0x39,
    0xc6,0xa5,0xb0,0x93,0x4a,0x8b,0x13,0x5f,0x2f,0x1f,0x95,0xe6,0xb7,0x42,0x69,0xfe,
    0x57,0xc4,0xbf,0xad,0x40,0x27,0x7f,0xa2,0xa7,0x75,0x80,0xbd,0xbb,0x1e,0x9f,0x6a,
    0x00,0x7a,0x23,0x26,0xf6,0x46,0x2a,0xc5,0x67,0xff,0xe4,0xa6,0xac,0x77,0x7a,0x46,
    0x38,0xbd,0x88,0x82,0x59,0xb3,0x1f,0x7e,0xdc,0xf0,0x7d,0x36,0x1a,0x96,0x1c,0xb2,
    0x75,0x07,0x3f,0xcb,0x42,0x10,0xf6,0x36,0xd4,0xa6,0x18,0xbf,0x92,0x9a,0xc5,0xe8,
    0x06,0x79,0x39,0x91,0x28,0x06,0xf3,0xa3,0xff,0x23,0x8c,0xce,0x3a,0x1f,0x2f,0xd7,
    0x31,0x70,0xdc,0x47,0x14,0x39,0x58,0x8e,0xec,0xd7,0xf3,0x76,0x0e,0x4e,0x2b,0xfc,
    0x8d,0x3e,0x91,0x63,0x1c,0xac,0x5c,0xb2,0xa5,0xee,0xf1,0xb7,0xd3,0xc2,0x9b,0x60,
    0x53,0x8e,0xdb,0x98,0xb8,0x93,0x84,0x3c,0x3c,0xd0,0x12,0x0a,0x11,0xc6,0x63,0x64,
    0xaf,0xb1,0xf4,0x55,0x88,0x38,0x78,0x3b,0xe2,0x86,0x53,0x7c,0xae,0x56,0x07,0x54,
    0x73,0xf9,0xa7,0xcb,0x10,0x91,0x98,0x1b,0x08,0x59,0x43,0x23,0x11,0x03,0x9b,0xa5,
    0xc2,0x3d,0x36,0xae,0x4c,0xc3,0x25,0x11,0xfa,0x47,0x28,0x52,0x4b,0xf9,0x45,0x5b,
    0x7d,0xfe,0xa4,0xf6,0x9a,0x4d,0x6c,0xf3,0x34,0xfd,0xe3,0x6b,0xfb,0xb2,0x5a,0x0d,
    0x51,0x96,0x15,0x2c,0x1c,0xa0,0x5f,0xbc,0x9e,0xdc,0x21,0x16,0x5e,0xb0,0x34,0x95,
    0x0a,0xb4,0x13,0xd0,0x66,0x02,0x63,0x99,0x2a,0xd2,0x70,0x59,0x5c,0xfb,0xd3,0xe3,
    0x07,0x8b,0x4b,0x14,0x90,0x2b,0x96,0xc9,0xb4,0x7f,0x06,0xee,0x77,0x75,0x58,0x38,
    0x6c,0x8c,0xe5,0xb2,0xae,0x63,0x3f,0xa0,0xb3,0x78,0xa4,0x72,0x29,0xe5,0xa9,0x27,
    0xb7,0xad,0xd6,0x46,0xa8,0x3b,0xcb,0x4f,0x4b,0x99,0x58,0xc1,0x84,0x73,0xb9,0x70,
    0x92,0x7b,0x97,0x8a,0x90,0x96,0xa0,0xbf,0x3f,0xea,0xe9,0x14,0xb8,0x60,0x9a,0xb2,
    0x52,0x13,0x53,0x69,0x7e,0x34,0xfc,0x84,0xa9,0xd2,0xed,0xd4,0x6b,0x38,0xfd,0x98,
    0x3e,0x12,0x90,0xdf,0xb0,0xbe,0x13,0x64,0xa2,0x32,0x44,0xc1,0x9e,0x69,0x53,0x6c,
    0x8b,0x21,0x5c,0x9a,0x4c,0x08,0x31,0x3a,0xc6,0x83,0x03,0x48,0x0d,0x21,
  };

  ci_ip_pkt_fmt* pkt;

  STATE_ALLOC(union pkt_buf, buf);
  STATE_ALLOC(struct onload_timestamp, ts);

  pkt = &buf->pkt;
  pkt_init(pkt);
  pkt_append(pkt, pcap, sizeof(pcap));
  STATE_STASH(buf);

  ci_rx_pkt_timestamp_cpacket(pkt, ts);
  STATE_CHECK(ts, sec, 0x5c9a4c08);
  STATE_CHECK(ts, nsec, 0x313ac683);
  STATE_CHECK(ts, nsec_frac, 0x536c8b);

  STATE_FREE(buf);
  STATE_FREE(ts);
}

/* Test cpacket timestamp using a packet used for testing wireshark's parser */
static void test_rx_pkt_timestamp_cpacket_pcap_wireshark(void)
{
  static const uint8_t pcap[] = {
    0xfe,0xff,0x20,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x08,0x00,0x45,0x00,
    0x02,0x07,0x0f,0x45,0x40,0x00,0x80,0x06,0x90,0x10,0x91,0xfe,0xa0,0xed,0x41,0xd0,
    0xe4,0xdf,0x0d,0x2c,0x00,0x50,0x38,0xaf,0xfe,0x14,0x11,0x4c,0x61,0x8c,0x50,0x18,
    0x25,0xbc,0xa9,0x58,0x00,0x00,0x47,0x45,0x54,0x20,0x2f,0x64,0x6f,0x77,0x6e,0x6c,
    0x6f,0x61,0x64,0x2e,0x68,0x74,0x6d,0x6c,0x20,0x48,0x54,0x54,0x50,0x2f,0x31,0x2e,
    0x31,0x0d,0x0a,0x48,0x6f,0x73,0x74,0x3a,0x20,0x77,0x77,0x77,0x2e,0x65,0x74,0x68,
    0x65,0x72,0x65,0x61,0x6c,0x2e,0x63,0x6f,0x6d,0x0d,0x0a,0x55,0x73,0x65,0x72,0x2d,
    0x41,0x67,0x65,0x6e,0x74,0x3a,0x20,0x4d,0x6f,0x7a,0x69,0x6c,0x6c,0x61,0x2f,0x35,
    0x2e,0x30,0x20,0x28,0x57,0x69,0x6e,0x64,0x6f,0x77,0x73,0x3b,0x20,0x55,0x3b,0x20,
    0x57,0x69,0x6e,0x64,0x6f,0x77,0x73,0x20,0x4e,0x54,0x20,0x35,0x2e,0x31,0x3b,0x20,
    0x65,0x6e,0x2d,0x55,0x53,0x3b,0x20,0x72,0x76,0x3a,0x31,0x2e,0x36,0x29,0x20,0x47,
    0x65,0x63,0x6b,0x6f,0x2f,0x32,0x30,0x30,0x34,0x30,0x31,0x31,0x33,0x0d,0x0a,0x41,
    0x63,0x63,0x65,0x70,0x74,0x3a,0x20,0x74,0x65,0x78,0x74,0x2f,0x78,0x6d,0x6c,0x2c,
    0x61,0x70,0x70,0x6c,0x69,0x63,0x61,0x74,0x69,0x6f,0x6e,0x2f,0x78,0x6d,0x6c,0x2c,
    0x61,0x70,0x70,0x6c,0x69,0x63,0x61,0x74,0x69,0x6f,0x6e,0x2f,0x78,0x68,0x74,0x6d,
    0x6c,0x2b,0x78,0x6d,0x6c,0x2c,0x74,0x65,0x78,0x74,0x2f,0x68,0x74,0x6d,0x6c,0x3b,
    0x71,0x3d,0x30,0x2e,0x39,0x2c,0x74,0x65,0x78,0x74,0x2f,0x70,0x6c,0x61,0x69,0x6e,
    0x3b,0x71,0x3d,0x30,0x2e,0x38,0x2c,0x69,0x6d,0x61,0x67,0x65,0x2f,0x70,0x6e,0x67,
    0x2c,0x69,0x6d,0x61,0x67,0x65,0x2f,0x6a,0x70,0x65,0x67,0x2c,0x69,0x6d,0x61,0x67,
    0x65,0x2f,0x67,0x69,0x66,0x3b,0x71,0x3d,0x30,0x2e,0x32,0x2c,0x2a,0x2f,0x2a,0x3b,
    0x71,0x3d,0x30,0x2e,0x31,0x0d,0x0a,0x41,0x63,0x63,0x65,0x70,0x74,0x2d,0x4c,0x61,
    0x6e,0x67,0x75,0x61,0x67,0x65,0x3a,0x20,0x65,0x6e,0x2d,0x75,0x73,0x2c,0x65,0x6e,
    0x3b,0x71,0x3d,0x30,0x2e,0x35,0x0d,0x0a,0x41,0x63,0x63,0x65,0x70,0x74,0x2d,0x45,
    0x6e,0x63,0x6f,0x64,0x69,0x6e,0x67,0x3a,0x20,0x67,0x7a,0x69,0x70,0x2c,0x64,0x65,
    0x66,0x6c,0x61,0x74,0x65,0x0d,0x0a,0x41,0x63,0x63,0x65,0x70,0x74,0x2d,0x43,0x68,
    0x61,0x72,0x73,0x65,0x74,0x3a,0x20,0x49,0x53,0x4f,0x2d,0x38,0x38,0x35,0x39,0x2d,
    0x31,0x2c,0x75,0x74,0x66,0x2d,0x38,0x3b,0x71,0x3d,0x30,0x2e,0x37,0x2c,0x2a,0x3b,
    0x71,0x3d,0x30,0x2e,0x37,0x0d,0x0a,0x4b,0x65,0x65,0x70,0x2d,0x41,0x6c,0x69,0x76,
    0x65,0x3a,0x20,0x33,0x30,0x30,0x0d,0x0a,0x43,0x6f,0x6e,0x6e,0x65,0x63,0x74,0x69,
    0x6f,0x6e,0x3a,0x20,0x6b,0x65,0x65,0x70,0x2d,0x61,0x6c,0x69,0x76,0x65,0x0d,0x0a,
    0x52,0x65,0x66,0x65,0x72,0x65,0x72,0x3a,0x20,0x68,0x74,0x74,0x70,0x3a,0x2f,0x2f,
    0x77,0x77,0x77,0x2e,0x65,0x74,0x68,0x65,0x72,0x65,0x61,0x6c,0x2e,0x63,0x6f,0x6d,
    0x2f,0x64,0x65,0x76,0x65,0x6c,0x6f,0x70,0x6d,0x65,0x6e,0x74,0x2e,0x68,0x74,0x6d,
    0x6c,0x0d,0x0a,0x0d,0x0a,0xb4,0xe7,0x1c,0xd1,0x00,0x00,0x04,0x20,0x2c,0x52,0xde,
    0x01,0x32,0x30,0x30,0x34,0x2d,0x30,0x35,0x2d,0x31,0x33,0x20,0x31,0x30,0x3a,0x31,
    0x37,0x3a,0x30,0x38,0x2e,0x32,0x32,0x32,0x35,0x33,0x34,0x31,0x30,0x37,0x20,0x55,
    0x54,0x43,0x00,0x00,0x00,0x00,0x00,0x02,0x1f,0xd1,0xa4,0x93,0x85,0xb5,0xde,0x34,
    0x6b,0x00,0x01,0x00,0x5f,0x40,0xa3,0x4b,0x24,0x0d,0x43,0x99,0xdb,0x02,0x30,0x39,
    0x7b
  };

  ci_ip_pkt_fmt* pkt;

  STATE_ALLOC(union pkt_buf, buf);
  STATE_ALLOC(struct onload_timestamp, ts);

  pkt = &buf->pkt;
  pkt_init(pkt);
  pkt_append(pkt, pcap, sizeof(pcap));
  STATE_STASH(buf);

  ci_rx_pkt_timestamp_cpacket(pkt, ts);
  STATE_CHECK(ts, sec, 0x40a34b24);
  STATE_CHECK(ts, nsec, 0x0d4399db);
  STATE_CHECK(ts, nsec_frac, 0x2c52de);

  STATE_FREE(buf);
  STATE_FREE(ts);
}

int main(void) {
  TEST_RUN(test_rx_pkt_timestamp_cpacket_none);
  TEST_RUN(test_rx_pkt_timestamp_cpacket_basic);
  TEST_RUN(test_rx_pkt_timestamp_cpacket_subnano);
  TEST_RUN(test_rx_pkt_timestamp_cpacket_many);
  TEST_RUN(test_rx_pkt_timestamp_cpacket_bogus);
  TEST_RUN(test_rx_pkt_timestamp_cpacket_pcap_metawatch);
  TEST_RUN(test_rx_pkt_timestamp_cpacket_pcap_wireshark);
  TEST_END();
}