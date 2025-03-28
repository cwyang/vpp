/*
 * Copyright (c) 2011-2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/**
 * @file
 * @brief BFD UDP transport layer implementation
 */
#include <vppinfra/types.h>
#include <vlibmemory/api.h>
#include <vlib/vlib.h>
#include <vlib/buffer.h>
#include <vnet/ip/format.h>
#include <vnet/ethernet/packet.h>
#include <vnet/udp/udp_local.h>
#include <vnet/udp/udp_packet.h>
#include <vnet/ip/lookup.h>
#include <vnet/ip/icmp46_packet.h>
#include <vnet/ip/ip4.h>
#include <vnet/ip/ip6.h>
#include <vnet/ip/ip6_packet.h>
#include <vnet/ip/ip6_link.h>
#include <vnet/adj/adj.h>
#include <vnet/adj/adj_nbr.h>
#include <vnet/dpo/receive_dpo.h>
#include <vnet/fib/fib_entry.h>
#include <vnet/fib/fib_table.h>
#include <vlib/stats/stats.h>
#include <vnet/bfd/bfd_debug.h>
#include <vnet/bfd/bfd_udp.h>
#include <vnet/bfd/bfd_main.h>
#include <vnet/bfd/bfd_api.h>
#include <vnet/bfd/bfd.api_enum.h>

#define F(sym, str)                                                           \
  STATIC_ASSERT ((int) BFD_ERROR_##sym == (int) BFD_UDP_ERROR_##sym,          \
		 "BFD error enums mismatch");
foreach_bfd_error (F)
#undef F
  STATIC_ASSERT ((int) BFD_N_ERROR <= (int) BFD_UDP_N_ERROR,
		 "BFD error enum sizes mismatch");

typedef struct
{
  bfd_main_t *bfd_main;
  /* hashmap - bfd session index by bfd key - used for CLI/API lookup, where
   * discriminator is unknown */
  mhash_t bfd_session_idx_by_bfd_key;
  /* convenience variable */
  vnet_main_t *vnet_main;
  /* flag indicating whether echo_source_sw_if_index holds a valid value */
  int echo_source_is_set;
  /* loopback interface used to get echo source ip */
  u32 echo_source_sw_if_index;
  /* log class */
  vlib_log_class_t log_class;
  /* number of active udp4 single-hop sessions */
  u32 udp4_sh_sessions_count;
  u32 udp4_sh_sessions_count_stat_seg_entry;
  /* number of active udp6 single-hop sessions */
  u32 udp6_sh_sessions_count;
  u32 udp6_sh_sessions_count_stat_seg_entry;
  /* number of active udp4 multi-hop sessions */
  u32 udp4_mh_sessions_count;
  u32 udp4_mh_sessions_count_stat_seg_entry;
  /* number of active udp6 multi-hop sessions */
  u32 udp6_mh_sessions_count;
  u32 udp6_mh_sessions_count_stat_seg_entry;
} bfd_udp_main_t;

static vlib_node_registration_t bfd_udp4_input_node;
static vlib_node_registration_t bfd_udp6_input_node;
static vlib_node_registration_t bfd_udp_echo4_input_node;
static vlib_node_registration_t bfd_udp_echo6_input_node;

bfd_udp_main_t bfd_udp_main;

void
bfd_udp_update_stat_segment_entry (u32 entry, u64 value)
{
  vlib_stats_segment_lock ();
  vlib_stats_set_gauge (entry, value);
  vlib_stats_segment_unlock ();
}

vnet_api_error_t
bfd_udp_set_echo_source (u32 sw_if_index)
{
  vnet_sw_interface_t *sw_if =
    vnet_get_sw_interface_or_null (bfd_udp_main.vnet_main, sw_if_index);
  if (sw_if)
    {
      bfd_udp_main.echo_source_sw_if_index = sw_if_index;
      bfd_udp_main.echo_source_is_set = 1;
      return 0;
    }
  return VNET_API_ERROR_BFD_ENOENT;
}

vnet_api_error_t
bfd_udp_del_echo_source ()
{
  bfd_udp_main.echo_source_sw_if_index = ~0;
  bfd_udp_main.echo_source_is_set = 0;
  return 0;
}

int
bfd_udp_is_echo_available (bfd_transport_e transport)
{
  if (!bfd_udp_main.echo_source_is_set)
    {
      BFD_DBG ("UDP echo source not set - echo not available");
      return 0;
    }
  /*
   * for the echo to work, we need a loopback interface with at least one
   * address with netmask length at most 31 (ip4) or 127 (ip6) so that we can
   * pick an unused address from that subnet
   */
  vnet_sw_interface_t *sw_if =
    vnet_get_sw_interface_or_null (bfd_udp_main.vnet_main,
				   bfd_udp_main.echo_source_sw_if_index);
  if (sw_if && sw_if->flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP)
    {
      if (BFD_TRANSPORT_UDP4 == transport)
	{
	  ip4_main_t *im = &ip4_main;
	  ip_interface_address_t *ia = NULL;
          foreach_ip_interface_address (&im->lookup_main, ia,
                                        bfd_udp_main.echo_source_sw_if_index,
                                        0 /* honor unnumbered */, ({
                                          if (ia->address_length <= 31)
                                            {
                                              return 1;
                                            }
                                        }));
	}
      else if (BFD_TRANSPORT_UDP6 == transport)
	{
	  ip6_main_t *im = &ip6_main;
	  ip_interface_address_t *ia = NULL;
          foreach_ip_interface_address (&im->lookup_main, ia,
                                        bfd_udp_main.echo_source_sw_if_index,
                                        0 /* honor unnumbered */, ({
                                          if (ia->address_length <= 127)
                                            {
                                              return 1;
                                            }
                                        }));
	}
    }
  BFD_DBG ("No usable IP address for UDP echo - echo not available");
  return 0;
}

static u16
bfd_udp_bs_idx_to_sport (u32 bs_idx)
{
  /* The source port MUST be in the range 49152 through 65535. The same UDP
   * source port number MUST be used for all BFD Control packets associated
   * with a particular session.  The source port number SHOULD be unique among
   * all BFD sessions on the system. If more than 16384 BFD sessions are
   * simultaneously active, UDP source port numbers MAY be reused on
   * multiple sessions, but the number of distinct uses of the same UDP
   * source port number SHOULD be minimized.
   */
  return 49152 + bs_idx % (65535 - 49152 + 1);
}

int
bfd_udp_get_echo_src_ip4 (ip4_address_t * addr)
{
  if (!bfd_udp_main.echo_source_is_set)
    {
      BFD_ERR ("cannot find ip4 address, echo source not set");
      return 0;
    }
  ip_interface_address_t *ia = NULL;
  ip4_main_t *im = &ip4_main;

  foreach_ip_interface_address (
      &im->lookup_main, ia, bfd_udp_main.echo_source_sw_if_index,
      0 /* honor unnumbered */, ({
        ip4_address_t *x =
            ip_interface_address_get_address (&im->lookup_main, ia);
        if (ia->address_length <= 31)
          {
            addr->as_u32 = clib_host_to_net_u32 (x->as_u32);
            /*
             * flip the last bit to get a different address, might be network,
             * we don't care ...
             */
            addr->as_u32 ^= 1;
            addr->as_u32 = clib_net_to_host_u32 (addr->as_u32);
            return 1;
          }
      }));
  BFD_ERR ("cannot find ip4 address, no usable address found");
  return 0;
}

int
bfd_udp_get_echo_src_ip6 (ip6_address_t * addr)
{
  if (!bfd_udp_main.echo_source_is_set)
    {
      BFD_ERR ("cannot find ip6 address, echo source not set");
      return 0;
    }
  ip_interface_address_t *ia = NULL;
  ip6_main_t *im = &ip6_main;

  foreach_ip_interface_address (
      &im->lookup_main, ia, bfd_udp_main.echo_source_sw_if_index,
      0 /* honor unnumbered */, ({
        ip6_address_t *x =
            ip_interface_address_get_address (&im->lookup_main, ia);
        if (ia->address_length <= 127)
          {
            *addr = *x;
            addr->as_u8[15] ^= 1; /* flip the last bit of the address */
            return 1;
          }
      }));
  BFD_ERR ("cannot find ip6 address, no usable address found");
  return 0;
}

void
bfd_udp_get_echo_source (int *is_set, u32 * sw_if_index,
			 int *have_usable_ip4, ip4_address_t * ip4,
			 int *have_usable_ip6, ip6_address_t * ip6)
{
  if (bfd_udp_main.echo_source_is_set)
    {
      *is_set = 1;
      *sw_if_index = bfd_udp_main.echo_source_sw_if_index;
      *have_usable_ip4 = bfd_udp_get_echo_src_ip4 (ip4);
      *have_usable_ip6 = bfd_udp_get_echo_src_ip6 (ip6);
    }
  else
    {
      *is_set = 0;
    }
}

int
bfd_add_udp4_transport (vlib_main_t * vm, u32 bi, const bfd_session_t * bs,
			int is_echo)
{
  const bfd_udp_session_t *bus = &bs->udp;
  const bfd_udp_key_t *key = &bus->key;
  vlib_buffer_t *b = vlib_get_buffer (vm, bi);

  b->flags |= VNET_BUFFER_F_LOCALLY_ORIGINATED;
  if (bs->hop_type == BFD_HOP_TYPE_SINGLE)
    {
      vnet_buffer (b)->ip.adj_index[VLIB_RX] = bus->adj_index;
      vnet_buffer (b)->ip.adj_index[VLIB_TX] = bus->adj_index;
    }
  vnet_buffer (b)->sw_if_index[VLIB_RX] = 0;
  vnet_buffer (b)->sw_if_index[VLIB_TX] = ~0;
  typedef struct
  {
    ip4_header_t ip4;
    udp_header_t udp;
  } ip4_udp_headers;
  ip4_udp_headers *headers = NULL;
  vlib_buffer_advance (b, -sizeof (*headers));
  headers = vlib_buffer_get_current (b);
  clib_memset (headers, 0, sizeof (*headers));
  headers->ip4.ip_version_and_header_length = 0x45;
  headers->ip4.ttl = 255;
  headers->ip4.protocol = IP_PROTOCOL_UDP;
  headers->udp.src_port =
    clib_host_to_net_u16 (bfd_udp_bs_idx_to_sport (bs->bs_idx));
  if (is_echo)
    {
      int rv;
      if (!(rv = bfd_udp_get_echo_src_ip4 (&headers->ip4.src_address)))
	{
	  return rv;
	}
      headers->ip4.dst_address.as_u32 = key->local_addr.ip4.as_u32;
      headers->udp.dst_port = clib_host_to_net_u16 (UDP_DST_PORT_bfd_echo4);
    }
  else
    {
      headers->ip4.src_address.as_u32 = key->local_addr.ip4.as_u32;
      headers->ip4.dst_address.as_u32 = key->peer_addr.ip4.as_u32;
      if (bs->hop_type == BFD_HOP_TYPE_MULTI)
	{
	  headers->udp.dst_port = clib_host_to_net_u16 (UDP_DST_PORT_bfd4_mh);
	}
      else
	{
	  headers->udp.dst_port = clib_host_to_net_u16 (UDP_DST_PORT_bfd4);
	}
    }

  /* fix ip length, checksum and udp length */
  const u16 ip_length = vlib_buffer_length_in_chain (vm, b);

  headers->ip4.length = clib_host_to_net_u16 (ip_length);
  headers->ip4.checksum = ip4_header_checksum (&headers->ip4);

  const u16 udp_length = ip_length - (sizeof (headers->ip4));
  headers->udp.length = clib_host_to_net_u16 (udp_length);
  return 1;
}

int
bfd_add_udp6_transport (vlib_main_t * vm, u32 bi, const bfd_session_t * bs,
			int is_echo)
{
  const bfd_udp_session_t *bus = &bs->udp;
  const bfd_udp_key_t *key = &bus->key;
  vlib_buffer_t *b = vlib_get_buffer (vm, bi);

  b->flags |= VNET_BUFFER_F_LOCALLY_ORIGINATED;
  if (bs->hop_type == BFD_HOP_TYPE_SINGLE)
    {
      vnet_buffer (b)->ip.adj_index[VLIB_RX] = bus->adj_index;
      vnet_buffer (b)->ip.adj_index[VLIB_TX] = bus->adj_index;
    }
  vnet_buffer (b)->sw_if_index[VLIB_RX] = 0;
  vnet_buffer (b)->sw_if_index[VLIB_TX] = 0;
  typedef struct
  {
    ip6_header_t ip6;
    udp_header_t udp;
  } ip6_udp_headers;
  ip6_udp_headers *headers = NULL;
  vlib_buffer_advance (b, -sizeof (*headers));
  headers = vlib_buffer_get_current (b);
  clib_memset (headers, 0, sizeof (*headers));
  headers->ip6.ip_version_traffic_class_and_flow_label =
    clib_host_to_net_u32 (0x6 << 28);
  headers->ip6.hop_limit = 255;
  headers->ip6.protocol = IP_PROTOCOL_UDP;
  headers->udp.src_port =
    clib_host_to_net_u16 (bfd_udp_bs_idx_to_sport (bs->bs_idx));
  if (is_echo)
    {
      int rv;
      if (!(rv = bfd_udp_get_echo_src_ip6 (&headers->ip6.src_address)))
	{
	  return rv;
	}
      clib_memcpy_fast (&headers->ip6.dst_address, &key->local_addr.ip6,
			sizeof (headers->ip6.dst_address));

      headers->udp.dst_port = clib_host_to_net_u16 (UDP_DST_PORT_bfd_echo6);
    }
  else
    {
      clib_memcpy_fast (&headers->ip6.src_address, &key->local_addr.ip6,
			sizeof (headers->ip6.src_address));
      clib_memcpy_fast (&headers->ip6.dst_address, &key->peer_addr.ip6,
			sizeof (headers->ip6.dst_address));
      if (bs->hop_type == BFD_HOP_TYPE_MULTI)
	{
	  headers->udp.dst_port = clib_host_to_net_u16 (UDP_DST_PORT_bfd6_mh);
	}
      else
	{
	  headers->udp.dst_port = clib_host_to_net_u16 (UDP_DST_PORT_bfd6);
	}
    }

  /* fix ip payload length and udp length */
  const u16 udp_length =
    vlib_buffer_length_in_chain (vm, b) - (sizeof (headers->ip6));
  headers->udp.length = clib_host_to_net_u16 (udp_length);
  headers->ip6.payload_length = headers->udp.length;

  /* IPv6 UDP checksum is mandatory */
  int bogus = 0;
  headers->udp.checksum =
    ip6_tcp_udp_icmp_compute_checksum (vm, b, &headers->ip6, &bogus);
  ASSERT (bogus == 0);
  if (headers->udp.checksum == 0)
    {
      headers->udp.checksum = 0xffff;
    }
  return 1;
}

static void
bfd_create_frame_to_next_node (vlib_main_t *vm, vlib_node_runtime_t *rt,
			       u32 bi, const bfd_session_t *bs, u32 next,
			       vlib_combined_counter_main_t *tx_counter)
{
  vlib_buffer_t *b = vlib_get_buffer (vm, bi);
  vlib_node_t *from_node = vlib_get_node (vm, rt->node_index);
  ASSERT (next < vec_len (from_node->next_nodes));
  u32 to_node_index = from_node->next_nodes[next];
  vlib_frame_t *f = vlib_get_frame_to_node (vm, to_node_index);
  u32 *to_next = vlib_frame_vector_args (f);
  to_next[0] = bi;
  f->n_vectors = 1;
  if (b->flags & VLIB_BUFFER_IS_TRACED)
    {
      f->frame_flags |= VLIB_NODE_FLAG_TRACE;
    }
  vlib_put_frame_to_node (vm, to_node_index, f);
  vlib_increment_combined_counter (tx_counter, vm->thread_index, bs->bs_idx, 1,
				   vlib_buffer_length_in_chain (vm, b));
}

int
bfd_udp_calc_next_node (const struct bfd_session_s *bs, u32 * next_node)
{
  vnet_main_t *vnm = vnet_get_main ();
  const bfd_udp_session_t *bus = &bs->udp;

  if (bs->hop_type == BFD_HOP_TYPE_MULTI)
    {
      switch (bs->transport)
	{
	case BFD_TRANSPORT_UDP4:
	  *next_node = BFD_TX_IP4_LOOKUP;
	  return 1;
	case BFD_TRANSPORT_UDP6:
	  *next_node = BFD_TX_IP6_LOOKUP;
	  return 1;
	default:
	  /* drop */
	  return 0;
	}
    }

  ip_adjacency_t *adj = adj_get (bus->adj_index);
  /* For single-hop, don't try to send the buffer if the interface is not up */
  if (!vnet_sw_interface_is_up (vnm, bus->key.sw_if_index))
    return 0;

  switch (adj->lookup_next_index)
    {
    case IP_LOOKUP_NEXT_ARP:
      switch (bs->transport)
	{
	case BFD_TRANSPORT_UDP4:
	  *next_node = BFD_TX_IP4_ARP;
	  return 1;
	case BFD_TRANSPORT_UDP6:
	  *next_node = BFD_TX_IP6_NDP;
	  return 1;
	}
      break;
    case IP_LOOKUP_NEXT_REWRITE:
      switch (bs->transport)
	{
	case BFD_TRANSPORT_UDP4:
	  *next_node = BFD_TX_IP4_REWRITE;
	  return 1;
	case BFD_TRANSPORT_UDP6:
	  *next_node = BFD_TX_IP6_REWRITE;
	  return 1;
	}
      break;
    case IP_LOOKUP_NEXT_MIDCHAIN:
      switch (bs->transport)
	{
	case BFD_TRANSPORT_UDP4:
	  *next_node = BFD_TX_IP4_MIDCHAIN;
	  return 1;
	case BFD_TRANSPORT_UDP6:
	  *next_node = BFD_TX_IP6_MIDCHAIN;
	  return 1;
	}
      break;
    default:
      /* drop */
      break;
    }
  return 0;
}

int
bfd_transport_udp4 (vlib_main_t *vm, vlib_node_runtime_t *rt, u32 bi,
		    const struct bfd_session_s *bs, int is_echo)
{
  u32 next_node;
  int rv = bfd_udp_calc_next_node (bs, &next_node);
  bfd_main_t *bm = bfd_udp_main.bfd_main;
  if (rv)
    {
      bfd_create_frame_to_next_node (vm, rt, bi, bs, next_node,
				     is_echo ? &bm->tx_echo_counter :
						     &bm->tx_counter);
    }
  return rv;
}

int
bfd_transport_udp6 (vlib_main_t *vm, vlib_node_runtime_t *rt, u32 bi,
		    const struct bfd_session_s *bs, int is_echo)
{
  u32 next_node;
  int rv = bfd_udp_calc_next_node (bs, &next_node);
  bfd_main_t *bm = bfd_udp_main.bfd_main;
  if (rv)
    {
      bfd_create_frame_to_next_node (vm, rt, bi, bs, next_node,
				     is_echo ? &bm->tx_echo_counter :
						     &bm->tx_counter);
    }
  return rv;
}

static bfd_session_t *
bfd_lookup_session (bfd_udp_main_t * bum, const bfd_udp_key_t * key)
{
  uword *p = mhash_get (&bum->bfd_session_idx_by_bfd_key, key);
  if (p)
    {
      return bfd_find_session_by_idx (bum->bfd_main, *p);
    }
  return 0;
}

static void
bfd_udp_key_init (bfd_udp_key_t * key, u32 sw_if_index,
		  const ip46_address_t * local_addr,
		  const ip46_address_t * peer_addr)
{
  clib_memset (key, 0, sizeof (*key));
  key->sw_if_index = sw_if_index & 0xFFFF;
  key->local_addr.as_u64[0] = local_addr->as_u64[0];
  key->local_addr.as_u64[1] = local_addr->as_u64[1];
  key->peer_addr.as_u64[0] = peer_addr->as_u64[0];
  key->peer_addr.as_u64[1] = peer_addr->as_u64[1];
}

static vnet_api_error_t
bfd_udp_add_session_internal (vlib_main_t *vm, bfd_udp_main_t *bum,
			      bool multihop, u32 sw_if_index,
			      u32 desired_min_tx_usec,
			      u32 required_min_rx_usec, u8 detect_mult,
			      const ip46_address_t *local_addr,
			      const ip46_address_t *peer_addr,
			      bfd_session_t **bs_out)
{
  /* get a pool entry and if we end up not needing it, give it back */
  bfd_transport_e t = BFD_TRANSPORT_UDP4;
  if (!ip46_address_is_ip4 (local_addr))
    {
      t = BFD_TRANSPORT_UDP6;
    }
  bfd_session_t *bs = bfd_get_session (bum->bfd_main, t);
  if (!bs)
    {
      return VNET_API_ERROR_BFD_EAGAIN;
    }
  bfd_udp_session_t *bus = &bs->udp;
  clib_memset (bus, 0, sizeof (*bus));
  bus->adj_index = ADJ_INDEX_INVALID;
  bfd_udp_key_t *key = &bus->key;
  bfd_udp_key_init (key, sw_if_index, local_addr, peer_addr);
  const bfd_session_t *tmp = bfd_lookup_session (bum, key);
  if (tmp)
    {
      vlib_log_err (bum->log_class,
		    "duplicate bfd-udp session, existing bs_idx=%d",
		    tmp->bs_idx);
      bfd_put_session (bum->bfd_main, bs);
      return VNET_API_ERROR_BFD_EEXIST;
    }
  mhash_set (&bum->bfd_session_idx_by_bfd_key, key, bs->bs_idx, NULL);
  BFD_DBG ("session created, bs_idx=%u, multihop=%u, sw_if_index=%d, "
	   "local=%U, peer=%U",
	   bs->bs_idx, multihop, key->sw_if_index, format_ip46_address,
	   &key->local_addr, IP46_TYPE_ANY, format_ip46_address,
	   &key->peer_addr, IP46_TYPE_ANY);
  vlib_log_info (bum->log_class, "create BFD session: %U",
		 format_bfd_session, bs);
  const ip46_address_t *peer =
    (vnet_sw_interface_is_p2p (vnet_get_main (), key->sw_if_index) ?
       &zero_addr :
       &key->peer_addr);
  if (BFD_TRANSPORT_UDP4 == t)
    {
      if (multihop)
	{
	  ++bum->udp4_mh_sessions_count;
	  bfd_udp_update_stat_segment_entry (
	    bum->udp4_mh_sessions_count_stat_seg_entry,
	    bum->udp4_mh_sessions_count);
	  if (1 == bum->udp4_mh_sessions_count)
	    {
	      udp_register_dst_port (vm, UDP_DST_PORT_bfd4_mh,
				     bfd_udp4_input_node.index, 1);
	    }
	}
      else
	{
	  bus->adj_index = adj_nbr_add_or_lock (
	    FIB_PROTOCOL_IP4, VNET_LINK_IP4, peer, key->sw_if_index);
	  BFD_DBG ("adj_nbr_add_or_lock(FIB_PROTOCOL_IP4, VNET_LINK_IP4, "
		   " %U, %d) returns %d",
		   format_ip46_address, peer, IP46_TYPE_ANY, key->sw_if_index,
		   bus->adj_index);
	  ++bum->udp4_sh_sessions_count;
	  bfd_udp_update_stat_segment_entry (
	    bum->udp4_sh_sessions_count_stat_seg_entry,
	    bum->udp4_sh_sessions_count);
	  if (1 == bum->udp4_sh_sessions_count)
	    {
	      udp_register_dst_port (vm, UDP_DST_PORT_bfd4,
				     bfd_udp4_input_node.index, 1);
	      udp_register_dst_port (vm, UDP_DST_PORT_bfd_echo4,
				     bfd_udp_echo4_input_node.index, 1);
	    }
	}
    }
  else
    {
      if (multihop)
	{
	  ++bum->udp6_mh_sessions_count;
	  bfd_udp_update_stat_segment_entry (
	    bum->udp6_mh_sessions_count_stat_seg_entry,
	    bum->udp6_mh_sessions_count);
	  if (1 == bum->udp6_mh_sessions_count)
	    {
	      udp_register_dst_port (vm, UDP_DST_PORT_bfd6_mh,
				     bfd_udp6_input_node.index, 0);
	    }
	}
      else
	{
	  bus->adj_index = adj_nbr_add_or_lock (
	    FIB_PROTOCOL_IP6, VNET_LINK_IP6, peer, key->sw_if_index);
	  BFD_DBG ("adj_nbr_add_or_lock(FIB_PROTOCOL_IP6, VNET_LINK_IP6, "
		   "%U, %d) returns %d",
		   format_ip46_address, peer, IP46_TYPE_ANY, key->sw_if_index,
		   bus->adj_index);
	  ++bum->udp6_sh_sessions_count;
	  bfd_udp_update_stat_segment_entry (
	    bum->udp6_sh_sessions_count_stat_seg_entry,
	    bum->udp6_sh_sessions_count);
	  if (1 == bum->udp6_sh_sessions_count)
	    {
	      udp_register_dst_port (vm, UDP_DST_PORT_bfd6,
				     bfd_udp6_input_node.index, 0);
	      udp_register_dst_port (vm, UDP_DST_PORT_bfd_echo6,
				     bfd_udp_echo6_input_node.index, 0);
	    }
	}
    }

  if (multihop)
    {
      bs->hop_type = BFD_HOP_TYPE_MULTI;
    }
  else
    {
      bs->hop_type = BFD_HOP_TYPE_SINGLE;
    }
  *bs_out = bs;
  return bfd_session_set_params (bum->bfd_main, bs, desired_min_tx_usec,
				 required_min_rx_usec, detect_mult);
}

static vnet_api_error_t
bfd_udp_validate_api_input (bool multihop, u32 sw_if_index,
			    const ip46_address_t *local_addr,
			    const ip46_address_t *peer_addr)
{
  bfd_udp_main_t *bum = &bfd_udp_main;
  if (!multihop)
    {
      vnet_sw_interface_t *sw_if =
	vnet_get_sw_interface_or_null (bfd_udp_main.vnet_main, sw_if_index);
      if (!sw_if)
	{
	  vlib_log_err (bum->log_class,
			"got NULL sw_if when getting interface by index %u",
			sw_if_index);
	  return VNET_API_ERROR_INVALID_SW_IF_INDEX;
	}
    }

  if (ip46_address_is_ip4 (local_addr))
    {
      if (!ip46_address_is_ip4 (peer_addr))
	{
	  vlib_log_err (bum->log_class,
			"IP family mismatch (local is ipv4, peer is ipv6)");
	  return VNET_API_ERROR_INVALID_ARGUMENT;
	}
    }
  else
    {
      if (ip46_address_is_ip4 (peer_addr))
	{
	  vlib_log_err (bum->log_class,
			"IP family mismatch (local is ipv6, peer is ipv4)");
	  return VNET_API_ERROR_INVALID_ARGUMENT;
	}
    }

  return 0;
}

static vnet_api_error_t
bfd_udp_find_session_by_api_input (bool multihop, u32 sw_if_index,
				   const ip46_address_t *local_addr,
				   const ip46_address_t *peer_addr,
				   bfd_session_t **bs_out)
{
  vnet_api_error_t rv =
    bfd_udp_validate_api_input (multihop, sw_if_index, local_addr, peer_addr);
  if (!rv)
    {
      bfd_udp_main_t *bum = &bfd_udp_main;
      bfd_udp_key_t key;
      bfd_udp_key_init (&key, sw_if_index, local_addr, peer_addr);
      bfd_session_t *bs = bfd_lookup_session (bum, &key);
      if (bs)
	{
	  *bs_out = bs;
	}
      else
	{
	  vlib_log_err (bum->log_class,
			"BFD session not found, multihop=%d, sw_if_index=%u, "
			"local=%U, peer=%U",
			multihop, sw_if_index, format_ip46_address, local_addr,
			IP46_TYPE_ANY, format_ip46_address, peer_addr,
			IP46_TYPE_ANY);
	  return VNET_API_ERROR_BFD_ENOENT;
	}
    }
  return rv;
}

static vnet_api_error_t
bfd_api_verify_common (bool multihop, u32 sw_if_index, u32 desired_min_tx_usec,
		       u8 detect_mult, const ip46_address_t *local_addr,
		       const ip46_address_t *peer_addr)
{
  bfd_udp_main_t *bum = &bfd_udp_main;
  vnet_api_error_t rv =
    bfd_udp_validate_api_input (multihop, sw_if_index, local_addr, peer_addr);
  if (rv)
    {
      return rv;
    }
  if (detect_mult < 1)
    {
      vlib_log_err (bum->log_class, "detect_mult < 1");
      return VNET_API_ERROR_INVALID_ARGUMENT;
    }
  if (desired_min_tx_usec < 1)
    {
      vlib_log_err (bum->log_class, "desired_min_tx_usec < 1");
      return VNET_API_ERROR_INVALID_ARGUMENT;
    }
  return 0;
}

static void
bfd_udp_del_session_internal (vlib_main_t * vm, bfd_session_t * bs)
{
  bfd_udp_main_t *bum = &bfd_udp_main;
  BFD_DBG ("free bfd-udp session, bs_idx=%d", bs->bs_idx);
  bfd_session_stop (bum->bfd_main, bs);
  mhash_unset (&bum->bfd_session_idx_by_bfd_key, &bs->udp.key, NULL);
  adj_unlock (bs->udp.adj_index);
  switch (bs->transport)
    {
    case BFD_TRANSPORT_UDP4:
      if (bs->hop_type == BFD_HOP_TYPE_MULTI)
	{
	  --bum->udp4_mh_sessions_count;
	  bfd_udp_update_stat_segment_entry (
	    bum->udp4_mh_sessions_count_stat_seg_entry,
	    bum->udp4_mh_sessions_count);
	  if (!bum->udp4_mh_sessions_count)
	    {
	      udp_unregister_dst_port (vm, UDP_DST_PORT_bfd4_mh, 1);
	    }
	}
      else
	{
	  --bum->udp4_sh_sessions_count;
	  bfd_udp_update_stat_segment_entry (
	    bum->udp4_sh_sessions_count_stat_seg_entry,
	    bum->udp4_sh_sessions_count);
	  if (!bum->udp4_sh_sessions_count)
	    {
	      udp_unregister_dst_port (vm, UDP_DST_PORT_bfd4, 1);
	      udp_unregister_dst_port (vm, UDP_DST_PORT_bfd_echo4, 1);
	    }
	}
      break;
    case BFD_TRANSPORT_UDP6:
      if (bs->hop_type == BFD_HOP_TYPE_MULTI)
	{
	  --bum->udp6_mh_sessions_count;
	  bfd_udp_update_stat_segment_entry (
	    bum->udp6_mh_sessions_count_stat_seg_entry,
	    bum->udp6_mh_sessions_count);
	  if (!bum->udp6_mh_sessions_count)
	    {
	      udp_unregister_dst_port (vm, UDP_DST_PORT_bfd6_mh, 0);
	    }
	}
      else
	{
	  --bum->udp6_sh_sessions_count;
	  bfd_udp_update_stat_segment_entry (
	    bum->udp6_sh_sessions_count_stat_seg_entry,
	    bum->udp6_sh_sessions_count);
	  if (!bum->udp6_sh_sessions_count)
	    {
	      udp_unregister_dst_port (vm, UDP_DST_PORT_bfd6, 0);
	      udp_unregister_dst_port (vm, UDP_DST_PORT_bfd_echo6, 0);
	    }
	}

      break;
    }
  bfd_put_session (bum->bfd_main, bs);
}

static vnet_api_error_t
bfd_udp_add_and_start_session (bool multihop, u32 sw_if_index,
			       const ip46_address_t *local_addr,
			       const ip46_address_t *peer_addr,
			       u32 desired_min_tx_usec,
			       u32 required_min_rx_usec, u8 detect_mult,
			       u8 is_authenticated, u32 conf_key_id,
			       u8 bfd_key_id)
{
  bfd_session_t *bs = NULL;
  vnet_api_error_t rv;

  rv = bfd_udp_add_session_internal (vlib_get_main (), &bfd_udp_main, multihop,
				     sw_if_index, desired_min_tx_usec,
				     required_min_rx_usec, detect_mult,
				     local_addr, peer_addr, &bs);

  if (!rv && is_authenticated)
    {
      rv = bfd_auth_activate (bs, conf_key_id, bfd_key_id,
			      0 /* is not delayed */);
      if (rv)
	{
	  bfd_udp_del_session_internal (vlib_get_main (), bs);
	}
    }
  if (!rv)
    {
      bfd_session_start (bfd_udp_main.bfd_main, bs);
    }

  return rv;
}

vnet_api_error_t
bfd_udp_add_session (bool multihop, u32 sw_if_index,
		     const ip46_address_t *local_addr,
		     const ip46_address_t *peer_addr, u32 desired_min_tx_usec,
		     u32 required_min_rx_usec, u8 detect_mult,
		     u8 is_authenticated, u32 conf_key_id, u8 bfd_key_id)
{
  bfd_main_t *bm = &bfd_main;
  bfd_lock (bm);

  vnet_api_error_t rv =
    bfd_api_verify_common (multihop, sw_if_index, desired_min_tx_usec,
			   detect_mult, local_addr, peer_addr);

  if (!rv)
    rv = bfd_udp_add_and_start_session (
      multihop, sw_if_index, local_addr, peer_addr, desired_min_tx_usec,
      required_min_rx_usec, detect_mult, is_authenticated, conf_key_id,
      bfd_key_id);

  bfd_unlock (bm);
  return rv;
}

vnet_api_error_t
bfd_udp_upd_session (bool multihop, u32 sw_if_index,
		     const ip46_address_t *local_addr,
		     const ip46_address_t *peer_addr, u32 desired_min_tx_usec,
		     u32 required_min_rx_usec, u8 detect_mult,
		     u8 is_authenticated, u32 conf_key_id, u8 bfd_key_id)
{
  bfd_main_t *bm = &bfd_main;
  bfd_lock (bm);

  vnet_api_error_t rv =
    bfd_api_verify_common (multihop, sw_if_index, desired_min_tx_usec,
			   detect_mult, local_addr, peer_addr);
  if (!rv)
    {
      bfd_session_t *bs = NULL;

      rv = bfd_udp_find_session_by_api_input (multihop, sw_if_index,
					      local_addr, peer_addr, &bs);
      if (VNET_API_ERROR_BFD_ENOENT == rv)
	rv = bfd_udp_add_and_start_session (
	  multihop, sw_if_index, local_addr, peer_addr, desired_min_tx_usec,
	  required_min_rx_usec, detect_mult, is_authenticated, conf_key_id,
	  bfd_key_id);
      else
	rv = bfd_session_set_params (bfd_udp_main.bfd_main, bs,
				     desired_min_tx_usec, required_min_rx_usec,
				     detect_mult);
    }

  bfd_unlock (bm);
  return rv;
}

vnet_api_error_t
bfd_udp_mod_session (bool multihop, u32 sw_if_index,
		     const ip46_address_t *local_addr,
		     const ip46_address_t *peer_addr, u32 desired_min_tx_usec,
		     u32 required_min_rx_usec, u8 detect_mult)
{
  bfd_session_t *bs = NULL;
  bfd_main_t *bm = &bfd_main;
  vnet_api_error_t error;
  bfd_lock (bm);
  vnet_api_error_t rv = bfd_udp_find_session_by_api_input (
    multihop, sw_if_index, local_addr, peer_addr, &bs);
  if (rv)
    {
      bfd_unlock (bm);
      return rv;
    }

  error = bfd_session_set_params (bfd_udp_main.bfd_main, bs,
				  desired_min_tx_usec, required_min_rx_usec,
				  detect_mult);
  bfd_unlock (bm);
  return error;
}

vnet_api_error_t
bfd_udp_del_session (bool multihop, u32 sw_if_index,
		     const ip46_address_t *local_addr,
		     const ip46_address_t *peer_addr)
{
  bfd_session_t *bs = NULL;
  bfd_main_t *bm = &bfd_main;
  bfd_lock (bm);
  vnet_api_error_t rv = bfd_udp_find_session_by_api_input (
    multihop, sw_if_index, local_addr, peer_addr, &bs);
  if (rv)
    {
      bfd_unlock (bm);
      return rv;
    }
  bfd_udp_del_session_internal (vlib_get_main (), bs);
  bfd_unlock (bm);
  return 0;
}

vnet_api_error_t
bfd_udp_session_set_flags (vlib_main_t *vm, bool multihop, u32 sw_if_index,
			   const ip46_address_t *local_addr,
			   const ip46_address_t *peer_addr, u8 admin_up_down)
{
  bfd_session_t *bs = NULL;
  bfd_main_t *bm = &bfd_main;
  bfd_lock (bm);
  vnet_api_error_t rv = bfd_udp_find_session_by_api_input (
    multihop, sw_if_index, local_addr, peer_addr, &bs);
  if (rv)
    {
      bfd_unlock (bm);
      return rv;
    }
  bfd_session_set_flags (vm, bs, admin_up_down);
  bfd_unlock (bm);
  return 0;
}

vnet_api_error_t
bfd_udp_auth_activate (bool multihop, u32 sw_if_index,
		       const ip46_address_t *local_addr,
		       const ip46_address_t *peer_addr, u32 conf_key_id,
		       u8 key_id, u8 is_delayed)
{
  bfd_main_t *bm = &bfd_main;
  bfd_lock (bm);
  vnet_api_error_t error;

  bfd_session_t *bs = NULL;
  vnet_api_error_t rv = bfd_udp_find_session_by_api_input (
    multihop, sw_if_index, local_addr, peer_addr, &bs);
  if (rv)
    {
      bfd_unlock (bm);
      return rv;
    }
  error = bfd_auth_activate (bs, conf_key_id, key_id, is_delayed);
  bfd_unlock (bm);
  return error;
}

vnet_api_error_t
bfd_udp_auth_deactivate (bool multihop, u32 sw_if_index,
			 const ip46_address_t *local_addr,
			 const ip46_address_t *peer_addr, u8 is_delayed)
{
  bfd_main_t *bm = &bfd_main;
  vnet_api_error_t error;
  bfd_lock (bm);
  bfd_session_t *bs = NULL;
  vnet_api_error_t rv = bfd_udp_find_session_by_api_input (
    multihop, sw_if_index, local_addr, peer_addr, &bs);
  if (rv)
    {
      bfd_unlock (bm);
      return rv;
    }
  error = bfd_auth_deactivate (bs, is_delayed);
  bfd_unlock (bm);
  return error;
}

typedef enum
{
  BFD_UDP_INPUT_NEXT_NORMAL,
  BFD_UDP_INPUT_NEXT_REPLY_ARP,
  BFD_UDP_INPUT_NEXT_REPLY_REWRITE,
  BFD_UDP_INPUT_NEXT_REPLY_MIDCHAIN,
  BFD_UDP_INPUT_N_NEXT,
} bfd_udp_input_next_t;

typedef enum
{
  BFD_UDP_ECHO_INPUT_NEXT_NORMAL,
  BFD_UDP_ECHO_INPUT_NEXT_REPLY_ARP,
  BFD_UDP_ECHO_INPUT_NEXT_REPLY_REWRITE,
  BFD_UDP_ECHO_INPUT_N_NEXT,
} bfd_udp_echo_input_next_t;

static_always_inline vl_counter_bfd_udp_enum_t
bfd_error_to_udp (bfd_error_t e)
{
  /* The UDP error is a super set of the proto independent errors */
  return ((vl_counter_bfd_udp_enum_t) e);
}

static void
bfd_udp4_find_headers (vlib_buffer_t * b, ip4_header_t ** ip4,
		       udp_header_t ** udp)
{
  /* sanity check first */
  const i32 start = vnet_buffer (b)->l3_hdr_offset;
  if (start < -(signed) sizeof (b->pre_data))
    {
      BFD_ERR ("Start of ip header is before pre_data, ignoring");
      *ip4 = NULL;
      *udp = NULL;
      return;
    }
  *ip4 = (ip4_header_t *) (b->data + start);
  if ((u8 *) * ip4 > (u8 *) vlib_buffer_get_current (b))
    {
      BFD_ERR ("Start of ip header is beyond current data, ignoring");
      *ip4 = NULL;
      *udp = NULL;
      return;
    }
  *udp = (udp_header_t *) ((*ip4) + 1);
}

static vl_counter_bfd_udp_enum_t
bfd_udp4_verify_transport (const ip4_header_t *ip4, const udp_header_t *udp,
			   const bfd_session_t *bs)
{
  const bfd_udp_session_t *bus = &bs->udp;
  const bfd_udp_key_t *key = &bus->key;
  if (ip4->src_address.as_u32 != key->peer_addr.ip4.as_u32)
    {
      BFD_ERR ("IPv4 src addr mismatch, got %U, expected %U",
	       format_ip4_address, ip4->src_address.as_u8, format_ip4_address,
	       key->peer_addr.ip4.as_u8);
      return BFD_UDP_ERROR_SRC_MISMATCH;
    }
  if (ip4->dst_address.as_u32 != key->local_addr.ip4.as_u32)
    {
      BFD_ERR ("IPv4 dst addr mismatch, got %U, expected %U",
	       format_ip4_address, ip4->dst_address.as_u8, format_ip4_address,
	       key->local_addr.ip4.as_u8);
      return BFD_UDP_ERROR_DST_MISMATCH;
    }

  // For single-hop, TTL must be 255
  if (bs->hop_type == BFD_HOP_TYPE_SINGLE)
    {
      const u8 expected_ttl = 255;
      if (ip4->ttl != expected_ttl)
	{
	  BFD_ERR ("IPv4 unexpected TTL value %u, expected %u", ip4->ttl,
		   expected_ttl);
	  return BFD_UDP_ERROR_TTL;
	}
    }

  if (clib_net_to_host_u16 (udp->src_port) < 49152)
    {
      BFD_ERR ("Invalid UDP src port %u, out of range <49152,65535>",
	       udp->src_port);
    }
  return BFD_UDP_ERROR_NONE;
}

typedef struct
{
  u32 bs_idx;
  bfd_pkt_t pkt;
} bfd_rpc_update_t;

static bfd_error_t
bfd_rpc_update_session (vlib_main_t *vm, u32 bs_idx, const bfd_pkt_t *pkt)
{
  bfd_main_t *bm = &bfd_main;
  bfd_error_t err;
  bfd_lock (bm);
  err = bfd_consume_pkt (vm, bm, pkt, bs_idx);
  bfd_unlock (bm);

  return err;
}

static vl_counter_bfd_udp_enum_t
bfd_udp4_scan (vlib_main_t *vm, vlib_buffer_t *b, bfd_session_t **bs_out)
{
  const bfd_pkt_t *pkt = vlib_buffer_get_current (b);
  if (sizeof (*pkt) > b->current_length)
    {
      BFD_ERR
	("Payload size %d too small to hold bfd packet of minimum size %d",
	 b->current_length, sizeof (*pkt));
      return BFD_UDP_ERROR_BAD;
    }
  ip4_header_t *ip4;
  udp_header_t *udp;
  bfd_udp4_find_headers (b, &ip4, &udp);
  if (!ip4 || !udp)
    {
      BFD_ERR ("Couldn't find ip4 or udp header");
      return BFD_UDP_ERROR_BAD;
    }
  const u32 udp_payload_length = udp->length - sizeof (*udp);
  if (pkt->head.length > udp_payload_length)
    {
      BFD_ERR
	("BFD packet length is larger than udp payload length (%u > %u)",
	 pkt->head.length, udp_payload_length);
      return BFD_UDP_ERROR_LENGTH;
    }
  vl_counter_bfd_udp_enum_t err;
  if (BFD_UDP_ERROR_NONE !=
      (err = bfd_error_to_udp (bfd_verify_pkt_common (pkt))))
    {
      return err;
    }
  bfd_session_t *bs = NULL;
  if (pkt->your_disc)
    {
      BFD_DBG ("Looking up BFD session using discriminator %u",
	       pkt->your_disc);
      bs = bfd_find_session_by_disc (bfd_udp_main.bfd_main, pkt->your_disc);
    }
  else
    {
      bfd_udp_key_t key;
      clib_memset (&key, 0, sizeof (key));
      if (udp->dst_port == clib_host_to_net_u16 (UDP_DST_PORT_bfd4_mh))
	{
	  key.sw_if_index = ~0;
	}
      else
	{
	  key.sw_if_index = vnet_buffer (b)->sw_if_index[VLIB_RX];
	}
      key.local_addr.ip4.as_u32 = ip4->dst_address.as_u32;
      key.peer_addr.ip4.as_u32 = ip4->src_address.as_u32;
      BFD_DBG ("Looking up BFD session using key (sw_if_index=%u, local=%U, "
	       "peer=%U)",
	       key.sw_if_index, format_ip4_address, key.local_addr.ip4.as_u8,
	       format_ip4_address, key.peer_addr.ip4.as_u8);
      bs = bfd_lookup_session (&bfd_udp_main, &key);
    }
  if (!bs)
    {
      BFD_ERR ("BFD session lookup failed - no session matches BFD pkt");
      return BFD_UDP_ERROR_NO_SESSION;
    }
  BFD_DBG ("BFD session found, bs_idx=%u", bs->bs_idx);
  if (!bfd_verify_pkt_auth (vm, pkt, b->current_length, bs))
    {
      BFD_ERR ("Packet verification failed, dropping packet");
      return BFD_UDP_ERROR_FAILED_VERIFICATION;
    }
  if (BFD_UDP_ERROR_NONE != (err = bfd_udp4_verify_transport (ip4, udp, bs)))
    {
      return err;
    }
  err = bfd_error_to_udp (bfd_rpc_update_session (vm, bs->bs_idx, pkt));
  *bs_out = bs;
  return err;
}

static void
bfd_udp6_find_headers (vlib_buffer_t * b, ip6_header_t ** ip6,
		       udp_header_t ** udp)
{
  /* sanity check first */
  const i32 start = vnet_buffer (b)->l3_hdr_offset;
  if (start < -(signed) sizeof (b->pre_data))
    {
      BFD_ERR ("Start of ip header is before pre_data, ignoring");
      *ip6 = NULL;
      *udp = NULL;
      return;
    }
  *ip6 = (ip6_header_t *) (b->data + start);
  if ((u8 *) * ip6 > (u8 *) vlib_buffer_get_current (b))
    {
      BFD_ERR ("Start of ip header is beyond current data, ignoring");
      *ip6 = NULL;
      *udp = NULL;
      return;
    }
  if ((*ip6)->protocol != IP_PROTOCOL_UDP)
    {
      BFD_ERR ("Unexpected protocol in IPv6 header '%u', expected '%u' (== "
	       "IP_PROTOCOL_UDP)", (*ip6)->protocol, IP_PROTOCOL_UDP);
      *ip6 = NULL;
      *udp = NULL;
      return;
    }
  *udp = (udp_header_t *) ((*ip6) + 1);
}

static vl_counter_bfd_udp_enum_t
bfd_udp6_verify_transport (const ip6_header_t *ip6, const udp_header_t *udp,
			   const bfd_session_t *bs)
{
  const bfd_udp_session_t *bus = &bs->udp;
  const bfd_udp_key_t *key = &bus->key;
  if (ip6->src_address.as_u64[0] != key->peer_addr.ip6.as_u64[0] &&
      ip6->src_address.as_u64[1] != key->peer_addr.ip6.as_u64[1])
    {
      BFD_ERR ("IP src addr mismatch, got %U, expected %U",
	       format_ip6_address, ip6, format_ip6_address,
	       &key->peer_addr.ip6);
      return BFD_UDP_ERROR_SRC_MISMATCH;
    }
  if (ip6->dst_address.as_u64[0] != key->local_addr.ip6.as_u64[0] &&
      ip6->dst_address.as_u64[1] != key->local_addr.ip6.as_u64[1])
    {
      BFD_ERR ("IP dst addr mismatch, got %U, expected %U",
	       format_ip6_address, ip6, format_ip6_address,
	       &key->local_addr.ip6);
      return BFD_UDP_ERROR_DST_MISMATCH;
    }

  // For single-hop, hop-limit must be 255
  if (bs->hop_type == BFD_HOP_TYPE_SINGLE)
    {
      const u8 expected_hop_limit = 255;
      if (ip6->hop_limit != expected_hop_limit)
	{
	  BFD_ERR ("IPv6 unexpected hop-limit value %u, expected %u",
		   ip6->hop_limit, expected_hop_limit);
	  return BFD_UDP_ERROR_TTL;
	}
    }

  if (clib_net_to_host_u16 (udp->src_port) < 49152)
    {
      BFD_ERR ("Invalid UDP src port %u, out of range <49152,65535>",
	       udp->src_port);
    }
  return BFD_UDP_ERROR_NONE;
}

static vl_counter_bfd_udp_enum_t
bfd_udp6_scan (vlib_main_t *vm, vlib_buffer_t *b, bfd_session_t **bs_out)
{
  const bfd_pkt_t *pkt = vlib_buffer_get_current (b);
  if (sizeof (*pkt) > b->current_length)
    {
      BFD_ERR
	("Payload size %d too small to hold bfd packet of minimum size %d",
	 b->current_length, sizeof (*pkt));
      return BFD_UDP_ERROR_BAD;
    }
  ip6_header_t *ip6;
  udp_header_t *udp;
  bfd_udp6_find_headers (b, &ip6, &udp);
  if (!ip6 || !udp)
    {
      BFD_ERR ("Couldn't find ip6 or udp header");
      return BFD_UDP_ERROR_BAD;
    }
  const u32 udp_payload_length = udp->length - sizeof (*udp);
  if (pkt->head.length > udp_payload_length)
    {
      BFD_ERR
	("BFD packet length is larger than udp payload length (%u > %u)",
	 pkt->head.length, udp_payload_length);
      return BFD_UDP_ERROR_BAD;
    }
  vl_counter_bfd_udp_enum_t err;
  if (BFD_UDP_ERROR_NONE !=
      (err = bfd_error_to_udp (bfd_verify_pkt_common (pkt))))
    {
      return err;
    }
  bfd_session_t *bs = NULL;
  if (pkt->your_disc)
    {
      BFD_DBG ("Looking up BFD session using discriminator %u",
	       pkt->your_disc);
      bs = bfd_find_session_by_disc (bfd_udp_main.bfd_main, pkt->your_disc);
    }
  else
    {
      bfd_udp_key_t key;
      clib_memset (&key, 0, sizeof (key));
      if (udp->dst_port == clib_host_to_net_u16 (UDP_DST_PORT_bfd6_mh))
	{
	  key.sw_if_index = ~0;
	}
      else
	{
	  key.sw_if_index = vnet_buffer (b)->sw_if_index[VLIB_RX];
	}
      key.local_addr.ip6.as_u64[0] = ip6->dst_address.as_u64[0];
      key.local_addr.ip6.as_u64[1] = ip6->dst_address.as_u64[1];
      key.peer_addr.ip6.as_u64[0] = ip6->src_address.as_u64[0];
      key.peer_addr.ip6.as_u64[1] = ip6->src_address.as_u64[1];
      BFD_DBG ("Looking up BFD session using discriminator %u",
	       pkt->your_disc);
      bs = bfd_find_session_by_disc (bfd_udp_main.bfd_main, pkt->your_disc);

      bs = bfd_lookup_session (&bfd_udp_main, &key);
    }
  if (!bs)
    {
      BFD_ERR ("BFD session lookup failed - no session matches BFD pkt");
      return BFD_UDP_ERROR_NO_SESSION;
    }
  BFD_DBG ("BFD session found, bs_idx=%u", bs->bs_idx);
  if (!bfd_verify_pkt_auth (vm, pkt, b->current_length, bs))
    {
      BFD_ERR ("Packet verification failed, dropping packet");
      return BFD_UDP_ERROR_FAILED_VERIFICATION;
    }
  if (BFD_UDP_ERROR_NONE != (err = bfd_udp6_verify_transport (ip6, udp, bs)))
    {
      return err;
    }
  err = bfd_error_to_udp (bfd_rpc_update_session (vm, bs->bs_idx, pkt));
  *bs_out = bs;
  return err;
}

/*
 * Process a frame of bfd packets
 * Expect 1 packet / frame
 */
static uword
bfd_udp_input (vlib_main_t * vm, vlib_node_runtime_t * rt,
	       vlib_frame_t * f, int is_ipv6)
{
  u32 n_left_from, *from;
  bfd_input_trace_t *t0;
  bfd_main_t *bm = &bfd_main;

  from = vlib_frame_vector_args (f);	/* array of buffer indices */
  n_left_from = f->n_vectors;	/* number of buffer indices */

  while (n_left_from > 0)
    {
      u32 bi0;
      vlib_buffer_t *b0;
      u32 next0, error0;

      bi0 = from[0];
      b0 = vlib_get_buffer (vm, bi0);

      bfd_session_t *bs = NULL;

      /* If this pkt is traced, snapshot the data */
      if (b0->flags & VLIB_BUFFER_IS_TRACED)
	{
	  u64 len;
	  t0 = vlib_add_trace (vm, rt, b0, sizeof (*t0));
	  len = (b0->current_length < sizeof (t0->data)) ? b0->current_length :
							   sizeof (t0->data);
	  t0->len = len;
	  clib_memcpy_fast (t0->data, vlib_buffer_get_current (b0), len);
	}

      /* scan this bfd pkt. error0 is the counter index to bmp */
      bfd_lock (bm);
      if (is_ipv6)
	{
	  error0 = bfd_udp6_scan (vm, b0, &bs);
	}
      else
	{
	  error0 = bfd_udp4_scan (vm, b0, &bs);
	}
      b0->error = rt->errors[error0];

      next0 = BFD_UDP_INPUT_NEXT_NORMAL;
      if (BFD_UDP_ERROR_NONE == error0)
	{
	  vlib_increment_combined_counter (
	    &bm->rx_counter, vm->thread_index, bs->bs_idx, 1,
	    vlib_buffer_length_in_chain (vm, b0));
	  /*
	   *  if everything went fine, check for poll bit, if present, re-use
	   *  the buffer and based on (now updated) session parameters, send
	   *  the final packet back
	   */
	  const bfd_pkt_t *pkt = vlib_buffer_get_current (b0);
	  if (bfd_pkt_get_poll (pkt))
	    {
	      b0->current_data = 0;
	      b0->current_length = 0;
	      bfd_init_final_control_frame (vm, b0, bs);
	      if (is_ipv6)
		{
		  vlib_node_increment_counter (vm, bfd_udp6_input_node.index,
					       error0, 1);
		}
	      else
		{
		  vlib_node_increment_counter (vm, bfd_udp4_input_node.index,
					       error0, 1);
		}

	      const bfd_udp_session_t *bus = &bs->udp;

	      if (bs->hop_type == BFD_HOP_TYPE_MULTI)
		{
		  next0 = BFD_UDP_INPUT_NEXT_REPLY_REWRITE;
		}
	      else
		{
		  ip_adjacency_t *adj = adj_get (bus->adj_index);
		  switch (adj->lookup_next_index)
		    {
		    case IP_LOOKUP_NEXT_ARP:
		      next0 = BFD_UDP_INPUT_NEXT_REPLY_ARP;
		      break;
		    case IP_LOOKUP_NEXT_REWRITE:
		      next0 = BFD_UDP_INPUT_NEXT_REPLY_REWRITE;
		      break;
		    case IP_LOOKUP_NEXT_MIDCHAIN:
		      next0 = BFD_UDP_INPUT_NEXT_REPLY_MIDCHAIN;
		      break;
		    default:
		      /* drop */
		      break;
		    }
		}
	    }
	}

      bfd_unlock (bm);
      vlib_set_next_frame_buffer (vm, rt, next0, bi0);

      from += 1;
      n_left_from -= 1;
    }

  return f->n_vectors;
}

static uword
bfd_udp4_input (vlib_main_t * vm, vlib_node_runtime_t * rt, vlib_frame_t * f)
{
  return bfd_udp_input (vm, rt, f, 0);
}

/*
 * bfd input graph node declaration
 */
VLIB_REGISTER_NODE (bfd_udp4_input_node, static) = {
  .function = bfd_udp4_input,
  .name = "bfd-udp4-input",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,

  .n_errors = BFD_UDP_N_ERROR,
  .error_counters = bfd_udp_error_counters,

  .format_trace = bfd_input_format_trace,

  .n_next_nodes = BFD_UDP_INPUT_N_NEXT,
  .next_nodes =
      {
              [BFD_UDP_INPUT_NEXT_NORMAL] = "error-drop",
              [BFD_UDP_INPUT_NEXT_REPLY_ARP] = "ip4-arp",
              [BFD_UDP_INPUT_NEXT_REPLY_REWRITE] = "ip4-lookup",
              [BFD_UDP_INPUT_NEXT_REPLY_MIDCHAIN] = "ip4-midchain",
      },
};

static uword
bfd_udp6_input (vlib_main_t * vm, vlib_node_runtime_t * rt, vlib_frame_t * f)
{
  return bfd_udp_input (vm, rt, f, 1);
}

VLIB_REGISTER_NODE (bfd_udp6_input_node, static) = {
  .function = bfd_udp6_input,
  .name = "bfd-udp6-input",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,

  .n_errors = BFD_UDP_N_ERROR,
  .error_counters = bfd_udp_error_counters,

  .format_trace = bfd_input_format_trace,

  .n_next_nodes = BFD_UDP_INPUT_N_NEXT,
  .next_nodes =
      {
              [BFD_UDP_INPUT_NEXT_NORMAL] = "error-drop",
              [BFD_UDP_INPUT_NEXT_REPLY_ARP] = "ip6-discover-neighbor",
              [BFD_UDP_INPUT_NEXT_REPLY_REWRITE] = "ip6-lookup",
              [BFD_UDP_INPUT_NEXT_REPLY_MIDCHAIN] = "ip6-midchain",
      },
};

/*
 * Process a frame of bfd echo packets
 * Expect 1 packet / frame
 */
static uword
bfd_udp_echo_input (vlib_main_t * vm, vlib_node_runtime_t * rt,
		    vlib_frame_t * f, int is_ipv6)
{
  u32 n_left_from, *from;
  bfd_input_trace_t *t0;
  bfd_main_t *bm = &bfd_main;

  from = vlib_frame_vector_args (f);	/* array of buffer indices */
  n_left_from = f->n_vectors;	/* number of buffer indices */

  while (n_left_from > 0)
    {
      u32 bi0;
      vlib_buffer_t *b0;
      u32 next0;

      bi0 = from[0];
      b0 = vlib_get_buffer (vm, bi0);

      /* If this pkt is traced, snapshot the data */
      if (b0->flags & VLIB_BUFFER_IS_TRACED)
	{
	  u64 len;
	  t0 = vlib_add_trace (vm, rt, b0, sizeof (*t0));
	  len = (b0->current_length < sizeof (t0->data)) ? b0->current_length
	    : sizeof (t0->data);
	  t0->len = len;
	  clib_memcpy_fast (t0->data, vlib_buffer_get_current (b0), len);
	}

      bfd_session_t *bs = NULL;
      bfd_lock (bm);
      if ((bs = bfd_consume_echo_pkt (vm, bfd_udp_main.bfd_main, b0)))
	{
	  b0->error = rt->errors[BFD_UDP_ERROR_NONE];
	  next0 = BFD_UDP_ECHO_INPUT_NEXT_NORMAL;
	}
      else
	{
	  /* loop back the packet */
	  b0->error = rt->errors[BFD_UDP_ERROR_NONE];
	  if (is_ipv6)
	    {
	      vlib_node_increment_counter (vm, bfd_udp_echo6_input_node.index,
					   BFD_UDP_ERROR_NONE, 1);
	    }
	  else
	    {
	      vlib_node_increment_counter (vm, bfd_udp_echo4_input_node.index,
					   BFD_UDP_ERROR_NONE, 1);
	    }
	  next0 = BFD_UDP_ECHO_INPUT_NEXT_REPLY_REWRITE;
	}

      bfd_unlock (bm);

      if (bs)
	{
	  vlib_increment_combined_counter (
	    &bm->rx_echo_counter, vm->thread_index, bs->bs_idx, 1,
	    vlib_buffer_length_in_chain (vm, b0));
	}

      vlib_set_next_frame_buffer (vm, rt, next0, bi0);

      from += 1;
      n_left_from -= 1;
    }

  return f->n_vectors;
}

static uword
bfd_udp_echo4_input (vlib_main_t * vm, vlib_node_runtime_t * rt,
		     vlib_frame_t * f)
{
  return bfd_udp_echo_input (vm, rt, f, 0);
}

u8 *
bfd_echo_input_format_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  const bfd_udp_echo_input_trace_t *t =
    va_arg (*args, bfd_udp_echo_input_trace_t *);
  if (t->len > STRUCT_SIZE_OF (bfd_pkt_t, head))
    {
      s = format (s, "BFD ECHO:\n");
      s = format (s, "    data: %U", format_hexdump, t->data, t->len);
    }

  return s;
}

/*
 * bfd input graph node declaration
 */
VLIB_REGISTER_NODE (bfd_udp_echo4_input_node, static) = {
  .function = bfd_udp_echo4_input,
  .name = "bfd-udp-echo4-input",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,

  .n_errors = BFD_UDP_N_ERROR,
  .error_counters = bfd_udp_error_counters,

  .format_trace = bfd_echo_input_format_trace,

  .n_next_nodes = BFD_UDP_ECHO_INPUT_N_NEXT,
  .next_nodes =
      {
              [BFD_UDP_ECHO_INPUT_NEXT_NORMAL] = "error-drop",
              [BFD_UDP_ECHO_INPUT_NEXT_REPLY_ARP] = "ip4-arp",
              [BFD_UDP_ECHO_INPUT_NEXT_REPLY_REWRITE] = "ip4-lookup",
      },
};

static uword
bfd_udp_echo6_input (vlib_main_t * vm, vlib_node_runtime_t * rt,
		     vlib_frame_t * f)
{
  return bfd_udp_echo_input (vm, rt, f, 1);
}

VLIB_REGISTER_NODE (bfd_udp_echo6_input_node, static) = {
  .function = bfd_udp_echo6_input,
  .name = "bfd-udp-echo6-input",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,

  .n_errors = BFD_UDP_N_ERROR,
  .error_counters = bfd_udp_error_counters,

  .format_trace = bfd_echo_input_format_trace,

  .n_next_nodes = BFD_UDP_ECHO_INPUT_N_NEXT,
  .next_nodes =
      {
              [BFD_UDP_ECHO_INPUT_NEXT_NORMAL] = "error-drop",
              [BFD_UDP_ECHO_INPUT_NEXT_REPLY_ARP] = "ip6-discover-neighbor",
              [BFD_UDP_ECHO_INPUT_NEXT_REPLY_REWRITE] = "ip6-lookup",
      },
};


static clib_error_t *
bfd_udp_sw_if_add_del (CLIB_UNUSED (vnet_main_t *vnm), u32 sw_if_index,
		       u32 is_create)
{
  u32 *to_be_freed = NULL;
  bfd_udp_main_t *bum = &bfd_udp_main;
  BFD_DBG ("sw_if_add_del called, sw_if_index=%u, is_create=%u", sw_if_index,
	   is_create);
  if (!is_create)
    {
      bfd_session_t *bs;
      pool_foreach (bs, bum->bfd_main->sessions)
	{
	  if (bs->transport != BFD_TRANSPORT_UDP4 &&
	      bs->transport != BFD_TRANSPORT_UDP6)
	    {
	      continue;
	    }
	  if (bs->hop_type == BFD_HOP_TYPE_MULTI)
	    {
	      continue;
	    }
	  if (bs->udp.key.sw_if_index != sw_if_index)
	    {
	      continue;
	    }
	  vec_add1 (to_be_freed, bs->bs_idx);
	}
    }
  u32 *bs_idx;
  vec_foreach (bs_idx, to_be_freed)
    {
      bfd_session_t *bs = pool_elt_at_index (bum->bfd_main->sessions, *bs_idx);
      vlib_log_notice (bum->log_class,
		       "removal of sw_if_index=%u forces removal of bfd "
		       "session with bs_idx=%u",
		       sw_if_index, bs->bs_idx);
      bfd_session_set_flags (vlib_get_main (), bs, 0);
      bfd_udp_del_session_internal (vlib_get_main (), bs);
    }
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (bfd_udp_sw_if_add_del);

clib_error_t *
bfd_udp_stats_init (bfd_udp_main_t *bum)
{
  const char *name4 = "/bfd/udp4/sessions";
  bum->udp4_sh_sessions_count_stat_seg_entry =
    vlib_stats_add_gauge ("%s", name4);

  vlib_stats_set_gauge (bum->udp4_sh_sessions_count_stat_seg_entry, 0);
  if (~0 == bum->udp4_sh_sessions_count_stat_seg_entry)
    {
      return clib_error_return (
	0, "Could not create stat segment entry for %s", name4);
    }
  const char *name6 = "/bfd/udp6/sessions";
  bum->udp6_sh_sessions_count_stat_seg_entry =
    vlib_stats_add_gauge ("%s", name6);

  vlib_stats_set_gauge (bum->udp6_sh_sessions_count_stat_seg_entry, 0);
  if (~0 == bum->udp6_sh_sessions_count_stat_seg_entry)
    {
      return clib_error_return (
	0, "Could not create stat segment entry for %s", name6);
    }

  const char *name4_mh = "/bfd/udp4/sessions_mh";
  bum->udp4_mh_sessions_count_stat_seg_entry =
    vlib_stats_add_gauge ("%s", name4_mh);

  vlib_stats_set_gauge (bum->udp4_mh_sessions_count_stat_seg_entry, 0);
  if (~0 == bum->udp4_mh_sessions_count_stat_seg_entry)
    {
      return clib_error_return (
	0, "Could not create stat segment entry for %s", name4_mh);
    }
  const char *name6_mh = "/bfd/udp6/sessions_mh";
  bum->udp6_mh_sessions_count_stat_seg_entry =
    vlib_stats_add_gauge ("%s", name6_mh);

  vlib_stats_set_gauge (bum->udp6_mh_sessions_count_stat_seg_entry, 0);
  if (~0 == bum->udp6_mh_sessions_count_stat_seg_entry)
    {
      return clib_error_return (
	0, "Could not create stat segment entry for %s", name6_mh);
    }

  return 0;
}

/*
 * setup function
 */
static clib_error_t *
bfd_udp_init (vlib_main_t * vm)
{
  bfd_udp_main.udp4_sh_sessions_count = 0;
  bfd_udp_main.udp6_sh_sessions_count = 0;
  bfd_udp_main.udp4_mh_sessions_count = 0;
  bfd_udp_main.udp6_mh_sessions_count = 0;
  mhash_init (&bfd_udp_main.bfd_session_idx_by_bfd_key, sizeof (uword),
	      sizeof (bfd_udp_key_t));
  bfd_udp_main.bfd_main = &bfd_main;
  bfd_udp_main.vnet_main = vnet_get_main ();
  bfd_udp_stats_init (&bfd_udp_main);

  bfd_udp_main.log_class = vlib_log_register_class ("bfd", "udp");
  vlib_log_debug (bfd_udp_main.log_class, "initialized");
  return 0;
}

VLIB_INIT_FUNCTION (bfd_udp_init);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
