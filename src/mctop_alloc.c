#include <mctop.h>

#define MA_DP printf

static int
mctopo_find_double_max(double* arr, const uint n)
{
  if (arr != NULL)
    {
      double max = arr[0];
      uint max_i = 0;
      for (int i = 1; i < n; i++)
	{
	  if (arr[i] > (1.1 * max))
	    {
	      max = arr[i];
	      max_i = i;
	    }
	}
      MA_DP("-- mctopo_find_double_max: choosing %d with bw %f GB/s\n", max_i, max);
      return max_i;
    }
  return 0;
}

/* smt_first < 0  : don't care */
/* smt_first = 0  : physical cores first */
/* smt_first > 0  : all smt thread of a core first */
static void
mctopo_socket_get_hwc_ids(socket_t* socket, uint* hwc_ids, const int smt_first)
{
  MA_DP("-- Socket #%u getting (smt %u): ", socket->id, smt_first);
  
  if (smt_first < 0 || socket->topo->is_smt == 0)
    {
      for (int i = 0; i < socket->n_hwcs; i++)
	{
	  hwc_ids[i] = socket->hwcs[i]->id;
	  MA_DP("%-2u ", hwc_ids[i]);
	}
    }
  else if (smt_first == 0)
    {
      uint idx = 0;
      for (int ht = 0; ht < socket->topo->n_hwcs_per_core; ht++)
	{
	  hwc_gs_t* gs = mctop_socket_get_first_gs_core(socket);
	  for (int i = 0; i < socket->n_cores; i++)
	    {
	      hwc_ids[idx] = gs->hwcs[ht]->id;
	      MA_DP("%-2u ", hwc_ids[idx]);
	      idx++;
	      gs = gs->next;
	    }
	}
    }
  else
    {
      uint idx = 0;
      hwc_gs_t* gs = mctop_socket_get_first_gs_core(socket);
      for (int i = 0; i < socket->n_cores; i++)
	{
	  for (int ht = 0; ht < socket->topo->n_hwcs_per_core; ht++)
	    {
	      hwc_ids[idx] = gs->hwcs[ht]->id;
	      MA_DP("%-2u ", hwc_ids[idx]);
	      idx++;
	    }
	  gs = gs->next;
	}
    }
  MA_DP("\n");
}

static void
mctopo_alloc_prep_min_lat(mctopo_alloc_t* alloc, uint smt_first)
{
  mctopo_t* topo = alloc->topo;
  uint alloc_full[topo->n_hwcs];

  uint socket_i = mctopo_find_double_max(topo->mem_bandwidths, topo->n_sockets);
  socket_t* socket = &topo->sockets[socket_i];
  socket_t* socket_start = socket;
  uint hwc_i = 0, sibling_i = 0;
  while (1)
    {
      mctopo_socket_get_hwc_ids(socket, alloc_full + hwc_i, smt_first);
      hwc_i += socket->n_hwcs;
      if (hwc_i >= alloc->n_hwcs)
	{
	  break;
	}

      assert(sibling_i < socket->n_siblings);
      socket = mctop_sibling_get_other_socket(socket_start->siblings[sibling_i], socket_start);
      MA_DP("-- -> Lat %u / BW %f to %u\n",
	    socket_start->siblings[sibling_i]->latency,
	    socket->mem_bandwidths[socket_start->local_node],
	    socket_start->id);
      sibling_i++;
    }

  for (int i = 0; i < alloc->n_hwcs; i++)
    {
      alloc->hwcs[i] = alloc_full[i];
    }
}

mctopo_alloc_t*
mctopo_alloc_create(mctopo_t* topo, const uint n_hwcs, mctopo_alloc_policy policy)
{
  mctopo_alloc_t* alloc = malloc(sizeof(mctopo_alloc_t) + (n_hwcs * sizeof(uint)));
  alloc->topo = topo;
  alloc->n_hwcs = n_hwcs;
  if (n_hwcs > topo->n_hwcs)
    {
      fprintf(stderr, "MCTOP Warning: Asking for %u hw contexts. This processor has %u contexts.\n",
	      n_hwcs, topo->n_hwcs);
      alloc->n_hwcs = topo->n_hwcs;
    }
  alloc->policy = policy;
  alloc->cur = 0;
  switch (policy)
    {
    case MCTOPO_ALLOC_MIN_LAT:
      mctopo_alloc_prep_min_lat(alloc, 1);
      break;
    case MCTOPO_ALLOC_MIN_LAT_CORES:
      mctopo_alloc_prep_min_lat(alloc, 0);
      break;
    }
  return alloc;
}

void
mctopo_alloc_free(mctopo_alloc_t* alloc)
{
  free(alloc);
}


#ifdef __sparc__		/* SPARC */
#  include <atomic.h>
#  define CAS_U64(a,b,c) atomic_cas_64(a,b,c)
#  define FAI_U32(a) (atomic_inc_32_nv(a) - 1)
#elif defined(__tile__)		/* TILER */
#  include <arch/atomic.h>
#  include <arch/cycle.h>
#  define FAI_U32(a) arch_atomic_increment(a)
#elif __x86_64__
#  define FAI_U32(a) __sync_fetch_and_add(a, 1)
#else
#  error "Unsupported Architecture"
#endif

/* pin to ALL hw contexts contained in alloc */
int
mctopo_alloc_pin_all(mctopo_alloc_t* alloc)
{
  struct bitmask* bmask = numa_bitmask_alloc(alloc->topo->n_hwcs);
  for (int i = 0; i < alloc->n_hwcs; i++)
    {
      bmask = numa_bitmask_setbit(bmask, alloc->hwcs[i]);
    }

  int ret = numa_sched_setaffinity(0, bmask);
  numa_bitmask_free(bmask);
  return ret;
}

/* pin to ONE hw context contained in alloc */
int
mctopo_alloc_pin(mctopo_alloc_t* alloc)
{
  uint hwcid = FAI_U32(&alloc->cur);
  if (hwcid < alloc->n_hwcs)
    {
      hwcid = alloc->hwcs[hwcid];
      int ret = mctop_set_cpu(hwcid);
      mctop_hwcid_fix_numa_node(alloc->topo, hwcid);
      return ret;
    }
  return 0;
}