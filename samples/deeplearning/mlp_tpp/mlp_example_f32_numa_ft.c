/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Alexander Heinecke (Intel Corp.)
******************************************************************************/
#include <libxsmm.h>
#include <libxsmm_sync.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#if defined(_OPENMP)
# include <omp.h>
#endif

#define CHECK_L1

/* include c-based dnn library */
#include "../common/dnn_common.h"

LIBXSMM_INLINE void my_init_buf(float* buf, size_t size, int initPos, int initOne)
{
  int i;
  zero_buf(buf, size);
  for (i = 0; i < (int)size; ++i) {
    buf[i] = (float)((initOne != 0) ? 1.0 : ((initPos != 0) ? libxsmm_rng_f64() : (0.05 - libxsmm_rng_f64()/10.0)));
  }
}

typedef enum my_eltwise_fuse {
  MY_ELTWISE_FUSE_NONE = 0,
  MY_ELTWISE_FUSE_BIAS = 1,
  MY_ELTWISE_FUSE_RELU = 2,
  MY_ELTWISE_FUSE_BIAS_RELU = MY_ELTWISE_FUSE_BIAS | MY_ELTWISE_FUSE_RELU
} my_eltwise_fuse;

typedef enum my_pass {
  MY_PASS_FWD   = 1,
  MY_PASS_BWD_D = 2,
  MY_PASS_BWD_W = 4,
  MY_PASS_BWD   = 6
} my_pass;

typedef struct my_opt_config {
  libxsmm_blasint C;
  libxsmm_blasint K;
  libxsmm_blasint bc;
  libxsmm_blasint bk;
  libxsmm_blasint threads;
  float           lr;
  size_t          scratch_size;
  libxsmm_barrier* barrier;
} my_opt_config;

typedef struct my_smax_fwd_config {
  libxsmm_blasint N;
  libxsmm_blasint C;
  libxsmm_blasint bn;
  libxsmm_blasint bc;
  libxsmm_blasint threads;
  size_t          scratch_size;
  libxsmm_barrier* barrier;
} my_smax_fwd_config;

typedef struct my_smax_bwd_config {
  libxsmm_blasint N;
  libxsmm_blasint C;
  libxsmm_blasint bn;
  libxsmm_blasint bc;
  libxsmm_blasint threads;
  size_t          scratch_size;
  float           loss_weight;
  libxsmm_barrier* barrier;
} my_smax_bwd_config;

typedef struct my_fc_fwd_config {
  libxsmm_blasint N;
  libxsmm_blasint C;
  libxsmm_blasint K;
  libxsmm_blasint bn;
  libxsmm_blasint bc;
  libxsmm_blasint bk;
  libxsmm_blasint threads;
  my_eltwise_fuse fuse_type;
  libxsmm_blasint fwd_bf;
  libxsmm_blasint fwd_2d_blocking;
  libxsmm_blasint fwd_row_teams;
  libxsmm_blasint fwd_column_teams;
  size_t          scratch_size;
  libxsmm_barrier* barrier;
  libxsmm_smmfunction_reducebatch_strd gemm_fwd;
  libxsmm_smmfunction_reducebatch_strd gemm_fwd2;
} my_fc_fwd_config;

typedef struct my_fc_bwd_config {
  libxsmm_blasint N;
  libxsmm_blasint C;
  libxsmm_blasint K;
  libxsmm_blasint bn;
  libxsmm_blasint bc;
  libxsmm_blasint bk;
  libxsmm_blasint threads;
  my_eltwise_fuse fuse_type;
  libxsmm_blasint bwd_bf;
  libxsmm_blasint bwd_2d_blocking;
  libxsmm_blasint bwd_row_teams;
  libxsmm_blasint bwd_column_teams;
  libxsmm_blasint upd_bf;
  libxsmm_blasint upd_2d_blocking;
  libxsmm_blasint upd_row_teams;
  libxsmm_blasint upd_column_teams;
  libxsmm_blasint ifm_subtasks;
  libxsmm_blasint ofm_subtasks;
  size_t          scratch_size;
  libxsmm_barrier* barrier;
  libxsmm_smmfunction_reducebatch_strd gemm_bwd;
  libxsmm_smmfunction_reducebatch_strd gemm_bwd2;
  libxsmm_smmfunction_reducebatch_strd gemm_upd;
  libxsmm_smmfunction_reducebatch_strd gemm_upd2;
  libxsmm_xtransfunction tr_kernel;
} my_fc_bwd_config;

my_fc_fwd_config setup_my_fc_fwd(libxsmm_blasint N, libxsmm_blasint C, libxsmm_blasint K, libxsmm_blasint bn,
                                 libxsmm_blasint bc, libxsmm_blasint bk, libxsmm_blasint threads, my_eltwise_fuse fuse_type) {
  my_fc_fwd_config res;
  libxsmm_blasint lda = bk;
  libxsmm_blasint ldb = bc;
  libxsmm_blasint ldc = bk;
  float alpha = 1.0f;
  float beta = 1.0f;
  float zerobeta = 0.0f;

  /* setting up some handle values */
  res.N = N;
  res.C = C;
  res.K = K;
  res.bn = bn;
  res.bc = bc;
  res.bk = bk;
  res.threads = threads;
  res.fuse_type = fuse_type;

  /* setup parallelization strategy */
  if (threads == 16) {
    res.fwd_bf = 1;
    res.fwd_2d_blocking = 1;
    res.fwd_row_teams = 2;
    res.fwd_column_teams = 8;
  } else {
    res.fwd_bf = 1;
    res.fwd_2d_blocking = 0;
    res.fwd_row_teams = 1;
    res.fwd_column_teams = 1;
  }

#if 0
  res.fwd_bf = atoi(getenv("FWD_BF"));
  res.fwd_2d_blocking = atoi(getenv("FWD_2D_BLOCKING"));
  res.fwd_row_teams = atoi(getenv("FWD_ROW_TEAMS"));
  res.fwd_column_teams = atoi(getenv("FWD_COLUMN_TEAMS"));
#endif

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* TPP creation */
  res.gemm_fwd  = libxsmm_smmdispatch_reducebatch_strd(res.bk, res.bn, res.bc,
                    res.bk*res.bc*sizeof(float), res.bc*res.bn*sizeof(float),
                    &lda, &ldb, &ldc, &alpha, &beta, NULL, NULL);
  if ( res.gemm_fwd == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd failed. Bailing...!\n");
    exit(-1);
  }
  res.gemm_fwd2 = libxsmm_smmdispatch_reducebatch_strd(res.bk, res.bn, res.bc,
                    res.bk*res.bc*sizeof(float), res.bc*res.bn*sizeof(float),
                    &lda, &ldb, &ldc, &alpha, &zerobeta, NULL, NULL);
  if ( res.gemm_fwd2 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_fwd2 failed. Bailing...!\n");
    exit(-1);
  }

  /* init scratch */
  res.scratch_size = 0;

  return res;
}

my_fc_bwd_config setup_my_fc_bwd(libxsmm_blasint N, libxsmm_blasint C, libxsmm_blasint K, libxsmm_blasint bn,
                                 libxsmm_blasint bc, libxsmm_blasint bk, libxsmm_blasint threads, my_eltwise_fuse fuse_type) {
  my_fc_bwd_config res;
  const libxsmm_trans_descriptor* tr_desc = 0;
  libxsmm_descriptor_blob blob;
  libxsmm_blasint lda = bc;
  libxsmm_blasint ldb = bk;
  libxsmm_blasint ldc = bc;
  float alpha = 1.0f;
  float beta = 1.0f;
  float zerobeta = 0.0f;
  int updflags = LIBXSMM_GEMM_FLAGS( 'N', 'T' );
  libxsmm_blasint updM;
  libxsmm_blasint updN;

  /* setting up some handle values */
  res.N = N;
  res.C = C;
  res.K = K;
  res.bn = bn;
  res.bc = bc;
  res.bk = bk;
  res.threads = threads;
  res.fuse_type = fuse_type;

  /* setup parallelization strategy */
  if (threads == 16) {
    res.bwd_bf = 1;
    res.bwd_2d_blocking = 1;
    res.bwd_row_teams = 2;
    res.bwd_column_teams = 8;
    res.upd_bf = 1;
    res.upd_2d_blocking = 0;
    res.upd_row_teams = 1;
    res.upd_column_teams = 1;
    res.ifm_subtasks = 1;
    res.ofm_subtasks = 1;
  } else {
    res.bwd_bf = 1;
    res.bwd_2d_blocking = 0;
    res.bwd_row_teams = 1;
    res.bwd_column_teams = 1;
    res.upd_bf = 1;
    res.upd_2d_blocking = 0;
    res.upd_row_teams = 1;
    res.upd_column_teams = 1;
    res.ifm_subtasks = 1;
    res.ofm_subtasks = 1;
  }

#if 0
  res.bwd_bf = atoi(getenv("BWD_BF"));
  res.bwd_2d_blocking = atoi(getenv("BWD_2D_BLOCKING"));
  res.bwd_row_teams = atoi(getenv("BWD_ROW_TEAMS"));
  res.bwd_column_teams = atoi(getenv("BWD_COLUMN_TEAMS"));
  res.upd_bf = atoi(getenv("UPD_BF"));
  res.upd_2d_blocking = atoi(getenv("UPD_2D_BLOCKING"));
  res.upd_row_teams = atoi(getenv("UPD_ROW_TEAMS"));
  res.upd_column_teams = atoi(getenv("UPD_COLUMN_TEAMS"));
  res.ifm_subtasks = atoi(getenv("IFM_SUBTASKS"));
  res.ofm_subtasks = atoi(getenv("OFM_SUBTASKS"));
#endif

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* TPP creation */
  /* BWD GEMM */
  res.gemm_bwd  = libxsmm_smmdispatch_reducebatch_strd(res.bc, res.bn, res.bk,
                    res.bk*res.bc*sizeof(float), res.bk*res.bn*sizeof(float),
                    &lda, &ldb, &ldc, &alpha, &beta, NULL, NULL);
  if ( res.gemm_bwd == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_bwd failed. Bailing...!\n");
    exit(-1);
  }
  res.gemm_bwd2 = libxsmm_smmdispatch_reducebatch_strd(res.bc, res.bn, res.bk,
                    res.bk*res.bc*sizeof(float), res.bk*res.bn*sizeof(float),
                    &lda, &ldb, &ldc, &alpha, &zerobeta, NULL, NULL);
  if ( res.gemm_bwd2 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_bwd2 failed. Bailing...!\n");
    exit(-1);
  }
  /* Transpose kernel used for weight transpose in bwd pass */
  tr_desc = libxsmm_trans_descriptor_init(&blob, sizeof(float), res.bk, res.bc, res.bc);
  res.tr_kernel = libxsmm_dispatch_trans(tr_desc);
  if ( res.tr_kernel == NULL ) {
    fprintf( stderr, "JIT for transpose TPP tr_kernel failed. Bailing...!\n");
    exit(-1);
  }
  /* UPD GEMM */
  lda = res.bk;
  ldb = res.bc;
  ldc = res.bk;
  updM = res.bk/res.ofm_subtasks;
  updN = res.bc/res.ifm_subtasks;
  res.gemm_upd = libxsmm_smmdispatch_reducebatch_strd(updM, updN, res.bn,
                   res.K*res.bn*sizeof(float), res.C*res.bn*sizeof(float),
                   &lda, &ldb, &ldc, &alpha, &beta, &updflags, NULL);
  if ( res.gemm_upd == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_upd failed. Bailing...!\n");
    exit(-1);
  }
  res.gemm_upd2 = libxsmm_smmdispatch_reducebatch_strd(updM, updN, res.bn,
                    res.K*res.bn*sizeof(float), res.C*res.bn*sizeof(float),
                    &lda, &ldb, &ldc, &alpha, &zerobeta, &updflags, NULL);
  if ( res.gemm_upd2 == NULL ) {
    fprintf( stderr, "JIT for BRGEMM TPP gemm_upd2 failed. Bailing...!\n");
    exit(-1);
  }

  /* init scratch */
  res.scratch_size =  sizeof(float) * ( (((size_t)res.C + (size_t)res.K) * (size_t)res.N) + ((size_t)res.C * (size_t)res.K) );

  return res;
}

my_opt_config setup_my_opt(libxsmm_blasint C, libxsmm_blasint K, libxsmm_blasint bc, libxsmm_blasint bk,
                           libxsmm_blasint threads, float lr) {
  my_opt_config res;

  /* setting up some handle values */
  res.C = C;
  res.K = K;
  res.bc = bc;
  res.bk = bk;
  res.threads = threads;
  res.lr = lr;

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* init scratch */
  res.scratch_size = 0;

  return res;
}

my_smax_fwd_config setup_my_smax_fwd(libxsmm_blasint N, libxsmm_blasint C, libxsmm_blasint bn, libxsmm_blasint bc,
                                     libxsmm_blasint threads) {
  my_smax_fwd_config res;

  /* setting up some handle values */
  res.C = C;
  res.N = N;
  res.bc = bc;
  res.bn = bn;
  res.threads = threads;

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* init scratch */
  res.scratch_size = 0;

  return res;
}

my_smax_bwd_config setup_my_smax_bwd(libxsmm_blasint N, libxsmm_blasint C, libxsmm_blasint bn, libxsmm_blasint bc,
                                     libxsmm_blasint threads, float loss_weight) {
  my_smax_bwd_config res;

  /* setting up some handle values */
  res.C = C;
  res.N = N;
  res.bc = bc;
  res.bn = bn;
  res.threads = threads;
  res.loss_weight = loss_weight;

  /* setting up the barrier */
  res.barrier = libxsmm_barrier_create(threads, 1);

  /* init scratch */
  res.scratch_size = 0;

  return res;
}

void my_fc_fwd_exec( my_fc_fwd_config cfg, const float* wt_ptr, const float* in_act_ptr, float* out_act_ptr,
                     const float* bias_ptr, unsigned char* relu_ptr, int start_tid, int my_tid, void* scratch ) {
  const libxsmm_blasint nBlocksIFm = cfg.C / cfg.bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / cfg.bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / cfg.bn;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could be run in parallel */
  const libxsmm_blasint work = nBlocksOFm * nBlocksMB;
  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ?
          (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

  /* loop variables */
  libxsmm_blasint mb1ofm1 = 0, mb1 = 0, ofm1 = 0, ifm1 = 0, mb2 = 0, ofm2 = 0;
  libxsmm_blasint im_tasks_per_thread = 0, in_tasks_per_thread = 0;
  libxsmm_blasint my_in_start = 0, my_in_end = 0, my_im_start = 0, my_im_end = 0;
  libxsmm_blasint my_row_id = 0, my_col_id = 0, row_teams = 0, column_teams = 0;

  LIBXSMM_VLA_DECL(4, float,           output, out_act_ptr, nBlocksOFm, cfg.bn, cfg.bk);
  LIBXSMM_VLA_DECL(4, const float,      input,  in_act_ptr, nBlocksIFm, cfg.bn, cfg.bc);
  LIBXSMM_VLA_DECL(4, const float,     filter,      wt_ptr, nBlocksIFm, cfg.bc, cfg.bk);
  LIBXSMM_VLA_DECL(2, const float,       bias,    bias_ptr,                     cfg.bk);
  LIBXSMM_VLA_DECL(4, unsigned char, relumask,    relu_ptr, nBlocksOFm, cfg.bn, cfg.bk);

  unsigned long long  blocks = nBlocksIFm;
  libxsmm_blasint CB_BLOCKS = nBlocksIFm, BF = 1;
  LIBXSMM_UNUSED( scratch );

  BF = cfg.fwd_bf;
  CB_BLOCKS = nBlocksIFm/BF;
  blocks = CB_BLOCKS;

  row_teams = cfg.fwd_row_teams;
  column_teams = cfg.fwd_column_teams;
  my_col_id = ltid % column_teams;
  my_row_id = ltid / column_teams;
  im_tasks_per_thread = LIBXSMM_UPDIV(nBlocksMB, row_teams);
  in_tasks_per_thread = LIBXSMM_UPDIV(nBlocksOFm, column_teams);
  my_im_start = LIBXSMM_MIN(my_row_id * im_tasks_per_thread, nBlocksMB);
  my_im_end = LIBXSMM_MIN((my_row_id+1) * im_tasks_per_thread, nBlocksMB);
  my_in_start = LIBXSMM_MIN(my_col_id * in_tasks_per_thread, nBlocksOFm);
  my_in_end = LIBXSMM_MIN((my_col_id+1) * in_tasks_per_thread, nBlocksOFm);

  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  if (cfg.fwd_2d_blocking == 1) {
    if (BF > 1) {
      for (ifm1 = 0; ifm1 < BF; ++ifm1) {
        for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
          for (mb1 = my_im_start; mb1 < my_im_end; ++mb1) {
            /* Initialize output slice */
            if ( ifm1 == 0 ) {
              if ( (cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS ) {
                for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
                  for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
                    LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = LIBXSMM_VLA_ACCESS(2, bias, ofm1, ofm2, cfg.bk);
                  }
                }
              } else {
                for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
                  for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
                    LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = (float)0;
                  }
                }
              }
            }
            /* BRGEMM */
            cfg.gemm_fwd( &LIBXSMM_VLA_ACCESS(4, filter, ofm1, ifm1*CB_BLOCKS, 0, 0, nBlocksIFm, cfg.bc, cfg.bk),
                &LIBXSMM_VLA_ACCESS(4, input,  mb1, ifm1*CB_BLOCKS, 0, 0, nBlocksIFm, cfg.bn, cfg.bc),
                &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk), &blocks);
            /* apply post BRGEMM fusion */
            if ( ifm1 == BF-1  ) {
              if ( (cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU ) {
                for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
                  for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
                    float l_cur_out = LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk);
                    LIBXSMM_VLA_ACCESS(4, relumask, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = (unsigned char)(( l_cur_out > (float)0 ) ? 1 : 0);
                    l_cur_out = (l_cur_out > (float)0) ? l_cur_out : (float)0;
                    LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = l_cur_out;
                  }
                }
              }
            }
          }
        }
      }
    } else {
      for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
        for (mb1 = my_im_start; mb1 < my_im_end; ++mb1) {
          if ( (cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS ) {
            for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
              for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
                LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = LIBXSMM_VLA_ACCESS(2, bias, ofm1, ofm2, cfg.bk);
              }
            }
            cfg.gemm_fwd( &LIBXSMM_VLA_ACCESS(4, filter, ofm1, 0, 0, 0, nBlocksIFm, cfg.bc, cfg.bk),
                &LIBXSMM_VLA_ACCESS(4, input,  mb1, 0, 0, 0, nBlocksIFm, cfg.bn, cfg.bc),
                &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk), &blocks);
          } else {
            cfg.gemm_fwd2( &LIBXSMM_VLA_ACCESS(4, filter, ofm1, 0, 0, 0, nBlocksIFm, cfg.bc, cfg.bk),
                &LIBXSMM_VLA_ACCESS(4, input,  mb1, 0,  0, 0, nBlocksIFm, cfg.bn, cfg.bc),
                &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk), &blocks);
          }
          /* post GEMM fusion */
          if ( (cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU ) {
            for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
              for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
                float l_cur_out = LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk);
                LIBXSMM_VLA_ACCESS(4, relumask, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = (unsigned char)(( l_cur_out > (float)0 ) ? 1 : 0);
                l_cur_out = ( l_cur_out > (float)0 ) ? l_cur_out : (float)0;
                LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = l_cur_out;
              }
            }
          }
        }
      }
    }
  } else {
    if (BF > 1) {
      for ( ifm1 = 0; ifm1 < BF; ++ifm1 ) {
        for ( mb1ofm1 = thr_begin; mb1ofm1 < thr_end; ++mb1ofm1 ) {
          mb1  = mb1ofm1%nBlocksMB;
          ofm1 = mb1ofm1/nBlocksMB;
          /* Initialize output slice */
          if ( ifm1 == 0 ) {
            if ( (cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS ) {
              for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
                for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
                  LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = LIBXSMM_VLA_ACCESS(2, bias, ofm1, ofm2, cfg.bk);
                }
              }
            } else {
              for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
                for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
                  LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = (float)0;
                }
              }
            }
          }
          /* BRGEMM */
          cfg.gemm_fwd( &LIBXSMM_VLA_ACCESS(4, filter, ofm1, ifm1*CB_BLOCKS, 0, 0, nBlocksIFm, cfg.bc, cfg.bk),
              &LIBXSMM_VLA_ACCESS(4, input,  mb1, ifm1*CB_BLOCKS, 0, 0, nBlocksIFm, cfg.bn, cfg.bc),
              &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk), &blocks);
          /* post GEMM fusion */
          if ( ifm1 == BF-1  ) {
            if ( (cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU ) {
              for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
                for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
                  float l_cur_out = LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk);
                  LIBXSMM_VLA_ACCESS(4, relumask, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = (unsigned char)(( l_cur_out > (float)0 ) ? 1 : 0);
                  l_cur_out = (l_cur_out > (float)0) ? l_cur_out : (float)0;
                  LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = l_cur_out;
                }
              }
            }
          }
        }
      }
    } else {
      for ( mb1ofm1 = thr_begin; mb1ofm1 < thr_end; ++mb1ofm1 ) {
        mb1  = mb1ofm1%nBlocksMB;
        ofm1 = mb1ofm1/nBlocksMB;
        if ( (cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS ) {
          for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
            for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
              LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = LIBXSMM_VLA_ACCESS(2, bias, ofm1, ofm2, cfg.bk);
            }
          }
          cfg.gemm_fwd( &LIBXSMM_VLA_ACCESS(4, filter, ofm1, 0, 0, 0, nBlocksIFm, cfg.bc, cfg.bk),
              &LIBXSMM_VLA_ACCESS(4, input,  mb1, 0,  0, 0, nBlocksIFm, cfg.bn, cfg.bc),
              &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk), &blocks);
        } else {
          cfg.gemm_fwd2( &LIBXSMM_VLA_ACCESS(4, filter, ofm1, 0, 0, 0, nBlocksIFm, cfg.bc, cfg.bk),
              &LIBXSMM_VLA_ACCESS(4, input,  mb1, 0,  0, 0, nBlocksIFm, cfg.bn, cfg.bc),
              &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, cfg.bn, cfg.bk), &blocks);
        }
        /* post GEMM fusion */
        if ( (cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU ) {
          for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
            for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
              float l_cur_out = LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk);
              LIBXSMM_VLA_ACCESS(4, relumask, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = (unsigned char)(( l_cur_out > (float)0 ) ? 1 : 0);
              l_cur_out = ( l_cur_out > (float)0 ) ? l_cur_out : (float)0;
              LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = l_cur_out;
            }
          }
        }
      }
    }
  }

  libxsmm_barrier_wait(cfg.barrier, ltid);
}

void my_fc_bwd_exec( my_fc_bwd_config cfg, const float* wt_ptr, float* din_act_ptr,
                     float* dout_act_ptr, float* dwt_ptr, const float* in_act_ptr,
                     float* dbias_ptr, const unsigned char* relu_ptr, my_pass pass, int start_tid, int my_tid, void* scratch ) {
  /* here we assume that input and output blocking is similar */
  const libxsmm_blasint bn = cfg.bn;
  const libxsmm_blasint bk = cfg.bk;
  const libxsmm_blasint bc = cfg.bc;
  const libxsmm_blasint nBlocksIFm = cfg.C / bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / bn;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;

  /* Transpose kernel to transpose filters  */
  libxsmm_xtransfunction tr_kernel = cfg.tr_kernel;

  /* number of tasks for transpose that could be run in parallel */
  const libxsmm_blasint eltwise_work = nBlocksOFm * nBlocksMB;
  /* compute chunk size */
  const libxsmm_blasint eltwise_chunksize = (eltwise_work % cfg.threads == 0) ? (eltwise_work / cfg.threads) : ((eltwise_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint eltwise_thr_begin = (ltid * eltwise_chunksize < eltwise_work) ? (ltid * eltwise_chunksize) : eltwise_work;
  const libxsmm_blasint eltwise_thr_end = ((ltid + 1) * eltwise_chunksize < eltwise_work) ? ((ltid + 1) * eltwise_chunksize) : eltwise_work;
  libxsmm_blasint mb1ofm1;

  /* number of tasks for transpose that could be run in parallel */
  const libxsmm_blasint dbias_work = nBlocksOFm;
  /* compute chunk size */
  const libxsmm_blasint dbias_chunksize = (dbias_work % cfg.threads == 0) ? (dbias_work / cfg.threads) : ((dbias_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint dbias_thr_begin = (ltid * dbias_chunksize < dbias_work) ? (ltid * dbias_chunksize) : dbias_work;
  const libxsmm_blasint dbias_thr_end = ((ltid + 1) * dbias_chunksize < dbias_work) ? ((ltid + 1) * dbias_chunksize) : dbias_work;

  /* loop variables */
  libxsmm_blasint ofm1 = 0, mb1 = 0, ofm2 = 0, mb2 = 0;

  float *grad_output_ptr = (((cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU) ? ((float*)scratch)+(cfg.C*cfg.K) : dout_act_ptr);
  LIBXSMM_VLA_DECL(4, const float, doutput_orig,    dout_act_ptr, nBlocksOFm, bn, bk);
  LIBXSMM_VLA_DECL(4,       float,      doutput, grad_output_ptr, nBlocksOFm, bn, bk);

  LIBXSMM_VLA_DECL(2,               float,    dbias, dbias_ptr,                     cfg.bk);
  LIBXSMM_VLA_DECL(4, const unsigned char, relumask,  relu_ptr, nBlocksOFm, cfg.bn, cfg.bk);

  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  if ( (cfg.fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU ) {
    for ( mb1ofm1 = eltwise_thr_begin; mb1ofm1 < eltwise_thr_end; ++mb1ofm1 ) {
      mb1  = mb1ofm1%nBlocksMB;
      ofm1 = mb1ofm1/nBlocksMB;

      for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
        for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
          float l_cur_out = LIBXSMM_VLA_ACCESS(4, doutput_orig, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk);
          l_cur_out = (LIBXSMM_VLA_ACCESS(4, relumask, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) != 0) ? l_cur_out : (float)0;
          LIBXSMM_VLA_ACCESS(4, doutput, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk) = l_cur_out;
        }
      }
    }

    /* wait for eltwise to finish */
    libxsmm_barrier_wait(cfg.barrier, ltid);
  }

  if ( (cfg.fuse_type & MY_ELTWISE_FUSE_BIAS) == MY_ELTWISE_FUSE_BIAS ) {
    for ( ofm1 = dbias_thr_begin; ofm1 < dbias_thr_end; ++ofm1 ) {
      for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
        LIBXSMM_VLA_ACCESS( 2, dbias, ofm1, ofm2, cfg.bk ) = 0.0f;
      }

      for ( mb1 = 0; mb1 < nBlocksMB; ++mb1 ) {
        for ( mb2 = 0; mb2 < cfg.bn; ++mb2 ) {
          for ( ofm2 = 0; ofm2 < cfg.bk; ++ofm2 ) {
            LIBXSMM_VLA_ACCESS( 2, dbias, ofm1, ofm2, cfg.bk ) += LIBXSMM_VLA_ACCESS(4,  doutput, mb1, ofm1, mb2, ofm2, nBlocksOFm, cfg.bn, cfg.bk);
          }
        }
      }
    }

    /* wait for eltwise to finish */
    libxsmm_barrier_wait(cfg.barrier, ltid);
  }

  if ( (pass & MY_PASS_BWD_D) == MY_PASS_BWD_D ) {
    const libxsmm_blasint use_2d_blocking = cfg.bwd_2d_blocking;

    /* number of tasks that could be run in parallel */
    const libxsmm_blasint work = nBlocksIFm * nBlocksMB;
    /* compute chunk size */
    const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
    /* compute thr_begin and thr_end */
    const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
    const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

    /* number of tasks for transpose that could be run in parallel */
    const libxsmm_blasint transpose_work = nBlocksIFm * nBlocksOFm;
    /* compute chunk size */
    const libxsmm_blasint transpose_chunksize = (transpose_work % cfg.threads == 0) ? (transpose_work / cfg.threads) : ((transpose_work / cfg.threads) + 1);
    /* compute thr_begin and thr_end */
    const libxsmm_blasint transpose_thr_begin = (ltid * transpose_chunksize < transpose_work) ? (ltid * transpose_chunksize) : transpose_work;
    const libxsmm_blasint transpose_thr_end = ((ltid + 1) * transpose_chunksize < transpose_work) ? ((ltid + 1) * transpose_chunksize) : transpose_work;

    /* loop variables */
    libxsmm_blasint ifm1 = 0, ifm2 = 0, ifm1ofm1 = 0, mb1ifm1 = 0;
    libxsmm_blasint im_tasks_per_thread = 0, in_tasks_per_thread = 0, my_in_start = 0, my_in_end = 0, my_im_start = 0, my_im_end = 0, my_row_id = 0, my_col_id = 0, row_teams = 0, column_teams = 0;

    LIBXSMM_VLA_DECL(4, const float,    filter,          wt_ptr, nBlocksIFm, bc, bk);
    LIBXSMM_VLA_DECL(4,       float,    dinput,     din_act_ptr, nBlocksIFm, bn, bc);
    LIBXSMM_VLA_DECL(4,       float, filter_tr, (float*)scratch, nBlocksOFm, bk, bc);

    unsigned long long  blocks = nBlocksOFm;
    libxsmm_blasint KB_BLOCKS = nBlocksOFm, BF = 1;
    BF = cfg.bwd_bf;
    KB_BLOCKS = nBlocksOFm/BF;
    blocks = KB_BLOCKS;

    if (use_2d_blocking == 1) {
      row_teams = cfg.bwd_row_teams;
      column_teams = cfg.bwd_column_teams;
      my_col_id = ltid % column_teams;
      my_row_id = ltid / column_teams;
      im_tasks_per_thread = LIBXSMM_UPDIV(nBlocksMB, row_teams);
      in_tasks_per_thread = LIBXSMM_UPDIV(nBlocksIFm, column_teams);
      my_im_start = LIBXSMM_MIN(my_row_id * im_tasks_per_thread, nBlocksMB);
      my_im_end = LIBXSMM_MIN((my_row_id+1) * im_tasks_per_thread, nBlocksMB);
      my_in_start = LIBXSMM_MIN(my_col_id * in_tasks_per_thread, nBlocksIFm);
      my_in_end = LIBXSMM_MIN((my_col_id+1) * in_tasks_per_thread, nBlocksIFm);
    }

    /* transpose weight */
    for (ifm1ofm1 = transpose_thr_begin; ifm1ofm1 < transpose_thr_end; ++ifm1ofm1) {
      const unsigned int ubk = (unsigned int)bk;
      const unsigned int ubc = (unsigned int)bc;
      ofm1 = ifm1ofm1 / nBlocksIFm;
      ifm1 = ifm1ofm1 % nBlocksIFm;
      tr_kernel(&LIBXSMM_VLA_ACCESS(4,    filter, ofm1, ifm1, 0, 0, nBlocksIFm, bc, bk), &ubk,
                &LIBXSMM_VLA_ACCESS(4, filter_tr, ifm1, ofm1, 0, 0, nBlocksOFm, bk, bc), &ubc);
    }

    /* wait for transpose to finish */
    libxsmm_barrier_wait(cfg.barrier, ltid);

    if (use_2d_blocking == 1) {
      if (BF > 1) {
        for ( ofm1 = 0; ofm1 < BF; ++ofm1 ) {
          for (ifm1 = my_in_start; ifm1 < my_in_end; ++ifm1) {
            for (mb1 = my_im_start; mb1 < my_im_end; ++mb1) {
              /* Initialize intermediate f32 tensor */
              if ( ofm1 == 0 ) {
                for ( mb2 = 0; mb2 < bn; ++mb2 ) {
                  for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
                    LIBXSMM_VLA_ACCESS(4, dinput, mb1, ifm1, mb2, ifm2, nBlocksIFm, bn, bc) = (float)0;
                  }
                }
              }
              cfg.gemm_bwd( &LIBXSMM_VLA_ACCESS(4, filter_tr, ifm1, ofm1*KB_BLOCKS, 0, 0, nBlocksOFm, bk, bc ),
                  &LIBXSMM_VLA_ACCESS(4, doutput,   mb1,  ofm1*KB_BLOCKS, 0, 0, nBlocksOFm, bn, bk),
                  &LIBXSMM_VLA_ACCESS(4, dinput,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc), &blocks);
            }
          }
        }
      } else {
        for (ifm1 = my_in_start; ifm1 < my_in_end; ++ifm1) {
          for (mb1 = my_im_start; mb1 < my_im_end; ++mb1) {
            cfg.gemm_bwd2( &LIBXSMM_VLA_ACCESS(4, filter_tr, ifm1, 0, 0, 0, nBlocksOFm, bk, bc),
                &LIBXSMM_VLA_ACCESS(4, doutput,   mb1,  0, 0, 0, nBlocksOFm, bn, bk),
                &LIBXSMM_VLA_ACCESS(4, dinput,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc), &blocks);
          }
        }
      }
    } else {
      if (BF > 1) {
        for ( ofm1 = 0; ofm1 < BF; ++ofm1 ) {
          for ( mb1ifm1 = thr_begin; mb1ifm1 < thr_end; ++mb1ifm1 ) {
            mb1  = mb1ifm1%nBlocksMB;
            ifm1 = mb1ifm1/nBlocksMB;
            /* Initialize intermediate f32 tensor */
            if ( ofm1 == 0 ) {
              for ( mb2 = 0; mb2 < bn; ++mb2 ) {
                for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
                  LIBXSMM_VLA_ACCESS(4, dinput, mb1, ifm1, mb2, ifm2, nBlocksIFm, bn, bc) = (float)0;
                }
             }
            }
            cfg.gemm_bwd( &LIBXSMM_VLA_ACCESS(4, filter_tr, ifm1, ofm1*KB_BLOCKS, 0, 0, nBlocksOFm, bk, bc ),
                &LIBXSMM_VLA_ACCESS(4, doutput,   mb1,  ofm1*KB_BLOCKS, 0, 0, nBlocksOFm, bn, bk),
                &LIBXSMM_VLA_ACCESS(4, dinput,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc), &blocks);
          }
        }
      } else {
        for ( mb1ifm1 = thr_begin; mb1ifm1 < thr_end; ++mb1ifm1 ) {
          mb1  = mb1ifm1%nBlocksMB;
          ifm1 = mb1ifm1/nBlocksMB;
          cfg.gemm_bwd2( &LIBXSMM_VLA_ACCESS(4, filter_tr, ifm1, 0, 0, 0, nBlocksOFm, bk, bc ),
              &LIBXSMM_VLA_ACCESS(4, doutput,   mb1,  0, 0, 0, nBlocksOFm, bn, bk),
              &LIBXSMM_VLA_ACCESS(4, dinput,    mb1,  ifm1, 0, 0, nBlocksIFm, bn, bc), &blocks);
        }
      }
    }

    libxsmm_barrier_wait(cfg.barrier, ltid);
  }

  if ( (pass & MY_PASS_BWD_W) == MY_PASS_BWD_W ) {
    /* number of tasks that could be run in parallel */
    const libxsmm_blasint ofm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ofm_subtasks;
    const libxsmm_blasint ifm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ifm_subtasks;
    const libxsmm_blasint bbk = (cfg.upd_2d_blocking == 1) ? bk : bk/ofm_subtasks;
    const libxsmm_blasint bbc = (cfg.upd_2d_blocking == 1) ? bc : bc/ifm_subtasks;
    const libxsmm_blasint work = nBlocksIFm * ifm_subtasks * nBlocksOFm * ofm_subtasks;
    const libxsmm_blasint Cck_work = nBlocksIFm * ifm_subtasks * ofm_subtasks;
    const libxsmm_blasint Cc_work = nBlocksIFm * ifm_subtasks;

    /* 2D blocking parameters  */
    libxsmm_blasint use_2d_blocking = cfg.upd_2d_blocking;
    libxsmm_blasint im_tasks_per_thread = 0, in_tasks_per_thread = 0, my_in_start = 0, my_in_end = 0, my_im_start = 0, my_im_end = 0, my_row_id = 0, my_col_id = 0, row_teams = 0, column_teams = 0;

    /* compute chunk size */
    const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
    /* compute thr_begin and thr_end */
    const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
    const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;
    libxsmm_blasint BF = cfg.upd_bf;

    /* loop variables */
    libxsmm_blasint ifm1ofm1 = 0, ifm1 = 0, ifm2 = 0, bfn = 0, ii = 0, jj = 0;

    /* Batch reduce related variables */
    unsigned long long  blocks = nBlocksMB/BF;

    LIBXSMM_VLA_DECL(4, const float,   input, in_act_ptr, nBlocksIFm, bn, bc);
    LIBXSMM_VLA_DECL(4,       float, dfilter,    dwt_ptr, nBlocksIFm, bc, bk);

    if (use_2d_blocking == 1) {
      row_teams = cfg.upd_row_teams;
      column_teams = cfg.upd_column_teams;
      my_col_id = ltid % column_teams;
      my_row_id = ltid / column_teams;
      im_tasks_per_thread = LIBXSMM_UPDIV(nBlocksIFm, row_teams);
      in_tasks_per_thread = LIBXSMM_UPDIV(nBlocksOFm, column_teams);
      my_im_start = LIBXSMM_MIN(my_row_id * im_tasks_per_thread, nBlocksIFm);
      my_im_end = LIBXSMM_MIN((my_row_id+1) * im_tasks_per_thread, nBlocksIFm);
      my_in_start = LIBXSMM_MIN(my_col_id * in_tasks_per_thread, nBlocksOFm);
      my_in_end = LIBXSMM_MIN((my_col_id+1) * in_tasks_per_thread, nBlocksOFm);
    }

    if (use_2d_blocking == 1) {
      if (BF == 1) {
        for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
          for (ifm1 = my_im_start; ifm1 < my_im_end; ++ifm1) {
            cfg.gemm_upd2(&LIBXSMM_VLA_ACCESS(4, doutput, 0, ofm1, 0, 0, nBlocksOFm, bn, bk),
                          &LIBXSMM_VLA_ACCESS(4, input,   0, ifm1, 0, 0, nBlocksIFm, bn, bc),
                          &LIBXSMM_VLA_ACCESS(4, dfilter, ofm1, ifm1, 0, 0, nBlocksIFm, bc, bk), &blocks);
          }
        }
      } else {
        for (bfn = 0; bfn < BF; bfn++) {
          for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
            for (ifm1 = my_im_start; ifm1 < my_im_end; ++ifm1) {
              /* initialize current work task to zero */
              if (bfn == 0) {
                for (ii = 0; ii<bc; ii++) {
                  for (jj = 0; jj<bk; jj++) {
                    LIBXSMM_VLA_ACCESS(4, dfilter, ofm1, ifm1, ii, jj, nBlocksIFm, bc, bk) = (float)0;
                  }
                }
              }
              cfg.gemm_upd( &LIBXSMM_VLA_ACCESS(4, doutput, bfn*blocks, ofm1, 0, 0, nBlocksOFm, bn, bk),
                            &LIBXSMM_VLA_ACCESS(4, input,   bfn*blocks, ifm1, 0, 0, nBlocksIFm, bn, bc),
                            &LIBXSMM_VLA_ACCESS(4, dfilter, ofm1, ifm1, 0, 0, nBlocksIFm, bc, bk), &blocks);
            }
          }
        }
      }
    } else {
      if (BF == 1) {
        for ( ifm1ofm1 = thr_begin; ifm1ofm1 < thr_end; ++ifm1ofm1 ) {
          ofm1 = ifm1ofm1 / Cck_work;
          ofm2 = (ifm1ofm1 % Cck_work) / Cc_work;
          ifm1 = ((ifm1ofm1 % Cck_work) % Cc_work) / ifm_subtasks;
          ifm2 = ((ifm1ofm1 % Cck_work) % Cc_work) % ifm_subtasks;

          cfg.gemm_upd2( &LIBXSMM_VLA_ACCESS(4, doutput, 0, ofm1, 0, ofm2*bbk, nBlocksOFm, bn, bk),
                         &LIBXSMM_VLA_ACCESS(4, input,   0, ifm1, 0, ifm2*bbc, nBlocksIFm, bn, bc),
                         &LIBXSMM_VLA_ACCESS(4, dfilter, ofm1, ifm1, ifm2*bbc, ofm2*bbk, nBlocksIFm, bc, bk), &blocks);
        }
      } else {
        for (bfn = 0; bfn < BF; bfn++) {
          for ( ifm1ofm1 = thr_begin; ifm1ofm1 < thr_end; ++ifm1ofm1 ) {
            ofm1 = ifm1ofm1 / Cck_work;
            ofm2 = (ifm1ofm1 % Cck_work) / Cc_work;
            ifm1 = ((ifm1ofm1 % Cck_work) % Cc_work) / ifm_subtasks;
            ifm2 = ((ifm1ofm1 % Cck_work) % Cc_work) % ifm_subtasks;

            /* initialize current work task to zero */
            if (bfn == 0) {
              for (ii = 0; ii<bbc; ii++) {
                for (jj = 0; jj<bbk; jj++) {
                  LIBXSMM_VLA_ACCESS(4, dfilter, ofm1, ifm1, ifm2*bbc+ii, ofm2*bbk+jj, nBlocksIFm, bc, bk) = (float)0;
                }
              }
            }

            cfg.gemm_upd( &LIBXSMM_VLA_ACCESS(4, doutput, bfn*blocks, ofm1, 0, ofm2*bbk, nBlocksOFm, bn, bk),
                          &LIBXSMM_VLA_ACCESS(4, input,   bfn*blocks, ifm1, 0, ifm2*bbc, nBlocksIFm, bn, bc),
                          &LIBXSMM_VLA_ACCESS(4, dfilter, ofm1, ifm1, ifm2*bbc, ofm2*bbk, nBlocksIFm, bc, bk), &blocks);
          }
        }
      }
    }

    libxsmm_barrier_wait(cfg.barrier, ltid);
  }
}

void my_opt_exec( my_opt_config cfg, float* wt_ptr, const float* delwt_ptr, int start_tid, int my_tid, void* scratch ) {
  /* loop counters */
  libxsmm_blasint i;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;

  /* number of tasks that could run in parallel for the filters */
  const libxsmm_blasint work = cfg.C * cfg.K;
  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

  libxsmm_blasint iv = ( (thr_end-thr_begin)/16 ) * 16; /* compute iterations which are vectorizable */
  __m512 vlr = _mm512_set1_ps( cfg.lr );

  /* lazy barrier init */
  libxsmm_barrier_init( cfg.barrier, ltid );

  for ( i = thr_begin; i < thr_begin+iv; i+=16 ) {
    _mm512_storeu_ps( wt_ptr+i, _mm512_sub_ps( _mm512_loadu_ps( wt_ptr+i ), _mm512_mul_ps( vlr, _mm512_loadu_ps( delwt_ptr + i ) ) ) ) ;
  }
  for ( i = thr_begin+iv; i < thr_end; ++i ) {
    wt_ptr[i] = wt_ptr[i] - (cfg.lr*delwt_ptr[i]);
  }

  libxsmm_barrier_wait( cfg.barrier, ltid );
}

void my_smax_fwd_exec( my_smax_fwd_config cfg, const float* in_act_ptr, float* out_act_ptr, const int* label_ptr, float* loss, int start_tid, int my_tid, void* scratch ) {
  libxsmm_blasint bn = cfg.bn;
  libxsmm_blasint Bn = cfg.N/cfg.bn;
  libxsmm_blasint bc = cfg.bc;
  libxsmm_blasint Bc = cfg.C/cfg.bc;

  /* loop counters */
  libxsmm_blasint i = 0;
  libxsmm_blasint img1, img2, ifm1, ifm2;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;

  /* number of tasks that could run in parallel for the batch */
  const libxsmm_blasint n_work = Bn * bn;
  /* compute chunk size */
  const libxsmm_blasint n_chunksize = (n_work % cfg.threads == 0) ? (n_work / cfg.threads) : ((n_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint n_thr_begin = (ltid * n_chunksize < n_work) ? (ltid * n_chunksize) : n_work;
  const libxsmm_blasint n_thr_end = ((ltid + 1) * n_chunksize < n_work) ? ((ltid + 1) * n_chunksize) : n_work;

  LIBXSMM_VLA_DECL(4,       float, output, out_act_ptr, Bc, bn, bc);
  LIBXSMM_VLA_DECL(4, const float,  input,  in_act_ptr, Bc, bn, bc);
  LIBXSMM_VLA_DECL(2,   const int,  label,   label_ptr,         bn);

  /* lazy barrier init */
  libxsmm_barrier_init( cfg.barrier, ltid );

  for ( i = n_thr_begin; i < n_thr_end; ++i ) {
    float max =        FLT_MIN;
    float sum_of_exp = 0.0f;

    img1 = i/bn;
    img2 = i%bn;

    /* set output to input and set compute max per image */
    for ( ifm1 = 0; ifm1 < Bc; ++ifm1 ) {
      for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
        LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) = LIBXSMM_VLA_ACCESS( 4, input, img1, ifm1, img2, ifm2, Bc, bn, bc );
        if ( LIBXSMM_VLA_ACCESS( 4, input, img1, ifm1, img2, ifm2, Bc, bn, bc ) > max ) {
          max = LIBXSMM_VLA_ACCESS( 4, input, img1, ifm1, img2, ifm2, Bc, bn, bc );
        }
      }
    }

    /* sum exp over outputs */
    for ( ifm1 = 0; ifm1 < Bc; ++ifm1 ) {
      for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
        LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) = (float)exp( (double)(LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) - max) );
        sum_of_exp += LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc );
      }
    }

    /* scale output */
    sum_of_exp = 1.0f/sum_of_exp;
    for ( ifm1 = 0; ifm1 < Bc; ++ifm1 ) {
      for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
        LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) = LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) * sum_of_exp;
      }
    }
  }

  libxsmm_barrier_wait( cfg.barrier, ltid );

  /* calculate loss single threaded */
  if ( ltid == 0 ) {
    (*loss) = 0.0f;
    for ( img1 = 0; img1 < Bn; ++img1 ) {
      for ( img2 = 0; img2 <bn; ++img2 ) {
        libxsmm_blasint ifm = (libxsmm_blasint)LIBXSMM_VLA_ACCESS( 2, label, img1, img2, bn );
        libxsmm_blasint ifm1b = ifm/bc;
        libxsmm_blasint ifm2b = ifm%bc;
        float val = ( LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1b, img2, ifm2b, Bc, bn, bc ) > FLT_MIN ) ? LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1b, img2, ifm2b, Bc, bn, bc ) : FLT_MIN;
        *loss = LIBXSMM_LOGF( val );
      }
    }
    *loss = ((-1.0f)*(*loss))/cfg.N;
  }

  libxsmm_barrier_wait( cfg.barrier, ltid );
}

void my_smax_bwd_exec( my_smax_bwd_config cfg, float* delin_act_ptr, const float* out_act_ptr, const int* label_ptr, int start_tid, int my_tid, void* scratch ) {
  libxsmm_blasint bn = cfg.bn;
  libxsmm_blasint Bn = cfg.N/cfg.bn;
  libxsmm_blasint bc = cfg.bc;
  libxsmm_blasint Bc = cfg.C/cfg.bc;

  /* loop counters */
  libxsmm_blasint i = 0;
  libxsmm_blasint img1, img2, ifm1, ifm2;

  float rcp_N = 1.0f/cfg.N;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could run in parallel for the batch */
  const libxsmm_blasint n_work = Bn * bn;
  /* compute chunk size */
  const libxsmm_blasint n_chunksize = (n_work % cfg.threads == 0) ? (n_work / cfg.threads) : ((n_work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint n_thr_begin = (ltid * n_chunksize < n_work) ? (ltid * n_chunksize) : n_work;
  const libxsmm_blasint n_thr_end = ((ltid + 1) * n_chunksize < n_work) ? ((ltid + 1) * n_chunksize) : n_work;

  LIBXSMM_VLA_DECL(4, const float, output,   out_act_ptr,  Bc, bn, bc);
  LIBXSMM_VLA_DECL(4,       float, dinput, delin_act_ptr,  Bc, bn, bc);
  LIBXSMM_VLA_DECL(2,   const int,  label,     label_ptr,          bn);

  /* lazy barrier init */
  libxsmm_barrier_init( cfg.barrier, ltid );

  for ( i = n_thr_begin; i < n_thr_end; ++i ) {
    img1 = i/bn;
    img2 = i%bn;

    /* set output to input and set compute max per image */
    for ( ifm1 = 0; ifm1 < Bc; ++ifm1 ) {
      for ( ifm2 = 0; ifm2 < bc; ++ifm2 ) {
        if ( (ifm1*Bc)+ifm2 == (libxsmm_blasint)LIBXSMM_VLA_ACCESS( 2, label, img1, img2, bn ) ) {
          LIBXSMM_VLA_ACCESS( 4, dinput, img1, ifm1, img2, ifm2, Bc, bn, bc ) =
            ( LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) - 1.0f ) * rcp_N * cfg.loss_weight;
        } else {
          LIBXSMM_VLA_ACCESS( 4, dinput, img1, ifm1, img2, ifm2, Bc, bn, bc ) =
            LIBXSMM_VLA_ACCESS( 4, output, img1, ifm1, img2, ifm2, Bc, bn, bc ) * rcp_N * cfg.loss_weight;
        }
      }
    }
  }

  libxsmm_barrier_wait( cfg.barrier, ltid );
}

void init_on_numa_node_fwd( my_fc_fwd_config cfg, const float* wt_ptr, int start_tid, int my_tid ) {
  const libxsmm_blasint nBlocksIFm = cfg.C / cfg.bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / cfg.bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / cfg.bn;

  const libxsmm_blasint OFM_shift = nBlocksIFm *  cfg.bc * cfg.bk;
  const libxsmm_blasint IFM_shift = cfg.bc * cfg.bk;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could be run in parallel */
  const libxsmm_blasint work = nBlocksOFm * nBlocksMB;
  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ?
          (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

  /* loop variables */
  libxsmm_blasint ofm1 = 0, ifm1 = 0;
  libxsmm_blasint in_tasks_per_thread = 0, my_in_start = 0, my_in_end = 0, my_col_id = 0, column_teams = 0;

  libxsmm_blasint BF = cfg.fwd_bf;
  libxsmm_blasint CB_BLOCKS = nBlocksIFm/BF;

  column_teams = cfg.fwd_column_teams;
  my_col_id = ltid % column_teams;
  in_tasks_per_thread = LIBXSMM_UPDIV(nBlocksOFm, column_teams);

  my_in_start = LIBXSMM_MIN(my_col_id * in_tasks_per_thread, nBlocksOFm);
  my_in_end = LIBXSMM_MIN((my_col_id+1) * in_tasks_per_thread, nBlocksOFm);

  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  if (cfg.fwd_2d_blocking == 1) {
    if (BF > 1) {
      for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
        int ofm_offset = ofm1 * OFM_shift;
        for (ifm1 = 0; ifm1 < BF; ++ifm1) {
          /* -> LIBXSMM_VLA_ACCESS(4, filter, ofm1, ifm1*CB_BLOCKS, 0, 0, nBlocksIFm, cfg.bc, cfg.bk) */
          int ifm_offset = ifm1 * CB_BLOCKS * IFM_shift;
          float *l_buf = (float*) wt_ptr + ofm_offset + ifm_offset;
          my_init_buf(l_buf, CB_BLOCKS * IFM_shift, 0, 0);
        }
      }
    } else {
      for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
        float *l_buf = (float*) wt_ptr + ofm1 * OFM_shift;
        my_init_buf(l_buf, OFM_shift, 0, 0);
      }
    }
  } else {
    int ofm_s = thr_begin / nBlocksMB;
    int ofm_e = thr_end / nBlocksMB;
    if (BF > 1) {
      for ( ofm1 = ofm_s; ofm1 < ofm_e; ++ofm1 ) {
        int ofm_offset = ofm1 * OFM_shift;
        for ( ifm1 = 0; ifm1 < BF; ++ifm1 ) {
          /* -> &LIBXSMM_VLA_ACCESS(4, filter, ofm1, ifm1*CB_BLOCKS, 0, 0, nBlocksIFm, cfg.bc, cfg.bk), */
          int ifm_offset = ifm1 * CB_BLOCKS * IFM_shift;
          float *l_buf = (float*) wt_ptr + ofm_offset + ifm_offset;
          my_init_buf(l_buf, CB_BLOCKS * IFM_shift, 0, 0);
        }
      }
    } else {
      for ( ofm1 = ofm_s; ofm1 < ofm_e; ++ofm1 ) {
          /* -> LIBXSMM_VLA_ACCESS(4, filter, ofm1, 0, 0, 0, nBlocksIFm, cfg.bc, cfg.bk), */
          float *l_buf = (float*) wt_ptr + ofm1 * OFM_shift;
          my_init_buf(l_buf, OFM_shift, 0, 0);
      }
    }
  }
  libxsmm_barrier_wait(cfg.barrier, ltid);
}

void init_on_numa_node_bwd_d ( my_fc_bwd_config cfg, float* filter_tr, int start_tid, int my_tid ) {
  /* here we assume that input and output blocking is similar */
  const libxsmm_blasint bn = cfg.bn;
  const libxsmm_blasint bk = cfg.bk;
  const libxsmm_blasint bc = cfg.bc;
  const libxsmm_blasint nBlocksIFm = cfg.C / bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / bn;

  const libxsmm_blasint OFM_shift = cfg.bc * cfg.bk;
  const libxsmm_blasint IFM_shift = nBlocksOFm * cfg.bc * cfg.bk;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could be run in parallel */
  const libxsmm_blasint work = nBlocksIFm * nBlocksMB;
  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

  libxsmm_blasint BF = cfg.bwd_bf;
  libxsmm_blasint KB_BLOCKS = nBlocksOFm/BF;
  unsigned long long blocks = KB_BLOCKS;

  libxsmm_blasint column_teams = cfg.bwd_column_teams;
  libxsmm_blasint my_col_id = ltid % column_teams;
  libxsmm_blasint in_tasks_per_thread = LIBXSMM_UPDIV(nBlocksIFm, column_teams);

  libxsmm_blasint my_in_start = LIBXSMM_MIN(my_col_id * in_tasks_per_thread, nBlocksIFm);
  libxsmm_blasint my_in_end = LIBXSMM_MIN((my_col_id+1) * in_tasks_per_thread, nBlocksIFm);

  int ifm1, ofm1;
  /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  if (cfg.bwd_2d_blocking == 1) {
    if (BF > 1) {
      for (ifm1 = my_in_start; ifm1 < my_in_end; ++ifm1) {
        int ifm_offset = ifm1 * IFM_shift;
        for ( ofm1 = 0; ofm1 < BF; ++ofm1 ) {
          /* -> &LIBXSMM_VLA_ACCESS(4, filter_tr, ifm1, ofm1*KB_BLOCKS, 0, 0, nBlocksOFm, bk, bc ), */
          int ofm_offset = ofm1 * KB_BLOCKS * OFM_shift;
          float *l_buf = (float*) filter_tr + ifm_offset + ofm_offset;
          my_init_buf(l_buf, KB_BLOCKS * OFM_shift, 0, 0);
        }
      }
    } else {
      for (ifm1 = my_in_start; ifm1 < my_in_end; ++ifm1) {
          /* -> &LIBXSMM_VLA_ACCESS(4, filter_tr, ifm1, 0, 0, 0, nBlocksOFm, bk, bc), */
        float *l_buf = (float*) filter_tr + ifm1 * IFM_shift;
        my_init_buf(l_buf, IFM_shift, 0, 0);
      }
    }
  } else {
    int ifm_s = thr_begin / nBlocksMB;
    int ifm_e = thr_end / nBlocksMB;
    if (BF > 1) {
      for ( ifm1 = ifm_s; ifm1 < ifm_e; ++ifm1 ) {
        int ifm_offset = ifm1 * IFM_shift;
        for ( ofm1 = 0; ofm1 < BF; ++ofm1 ) {
          /* -> &LIBXSMM_VLA_ACCESS(4, filter_tr, ifm1, ofm1*KB_BLOCKS, 0, 0, nBlocksOFm, bk, bc ), */
          int ofm_offset = ofm1 * KB_BLOCKS * OFM_shift;
          float *l_buf = (float*) filter_tr + ifm_offset + ofm_offset;
          my_init_buf(l_buf, KB_BLOCKS * OFM_shift, 0, 0);
        }
      }
    } else {
      for ( ifm1 = ifm_s; ifm1 < ifm_e; ++ifm1 ) {
        /* -> &LIBXSMM_VLA_ACCESS(4, filter_tr, ifm1, 0, 0, 0, nBlocksOFm, bk, bc ), */
        float *l_buf = (float*) filter_tr + ifm1 * IFM_shift;
        my_init_buf(l_buf, IFM_shift, 0, 0);
      }
    }
  }

  libxsmm_barrier_wait(cfg.barrier, ltid);
}

void init_on_numa_node_bwd_w ( my_fc_bwd_config cfg, float *doutput, int start_tid, int my_tid ) {
  const libxsmm_blasint bn = cfg.bn;
  const libxsmm_blasint bk = cfg.bk;
  const libxsmm_blasint bc = cfg.bc;
  const libxsmm_blasint nBlocksIFm = cfg.C / bc;
  const libxsmm_blasint nBlocksOFm = cfg.K / bk;
  const libxsmm_blasint nBlocksMB  = cfg.N / bn;

  const libxsmm_blasint OFM_shift = cfg.bn * cfg.bk;
  const libxsmm_blasint N_shift = nBlocksOFm * cfg.bn * cfg.bk;

  /* computing first logical thread */
  const libxsmm_blasint ltid = my_tid - start_tid;
  /* number of tasks that could be run in parallel */
  const libxsmm_blasint ofm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ofm_subtasks;
  const libxsmm_blasint ifm_subtasks = (cfg.upd_2d_blocking == 1) ? 1 : cfg.ifm_subtasks;
  const libxsmm_blasint bbk = (cfg.upd_2d_blocking == 1) ? bk : bk/ofm_subtasks;
  const libxsmm_blasint work = nBlocksIFm * ifm_subtasks * nBlocksOFm * ofm_subtasks;
  const libxsmm_blasint Cck_work = nBlocksIFm * ifm_subtasks * ofm_subtasks;
  const libxsmm_blasint Cc_work = nBlocksIFm * ifm_subtasks;

  /* 2D blocking parameters  */
  libxsmm_blasint use_2d_blocking = cfg.upd_2d_blocking;
  libxsmm_blasint in_tasks_per_thread = 0, my_in_start = 0, my_in_end = 0, my_col_id = 0, column_teams = 0;

  /* compute chunk size */
  const libxsmm_blasint chunksize = (work % cfg.threads == 0) ? (work / cfg.threads) : ((work / cfg.threads) + 1);
  /* compute thr_begin and thr_end */
  const libxsmm_blasint thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
  const libxsmm_blasint thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;
  libxsmm_blasint BF = cfg.upd_bf;

  /* loop variables */
  libxsmm_blasint ifm1ofm1 = 0, ifm1 = 0, ifm2 = 0, bfn = 0, ii = 0, jj = 0;

  /* Batch reduce related variables */
  unsigned long long  blocks = nBlocksMB/BF;

  if (use_2d_blocking == 1) {
    column_teams = cfg.upd_column_teams;
    my_col_id = ltid % column_teams;
    in_tasks_per_thread = LIBXSMM_UPDIV(nBlocksOFm, column_teams);

    my_in_start = LIBXSMM_MIN(my_col_id * in_tasks_per_thread, nBlocksOFm);
    my_in_end = LIBXSMM_MIN((my_col_id+1) * in_tasks_per_thread, nBlocksOFm);
  }

    /* lazy barrier init */
  libxsmm_barrier_init(cfg.barrier, ltid);

  int ofm1, ofm2;
  if (use_2d_blocking == 1) {
    if (BF == 1) {
      for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
        /* -> &LIBXSMM_VLA_ACCESS(4, doutput, 0, ofm1, 0, 0, nBlocksOFm, bn, bk) */
        float *l_buf = (float*) doutput + ofm1 * OFM_shift;
        my_init_buf(l_buf, OFM_shift, 0, 0);
      }
    } else {
      for (bfn = 0; bfn < BF; bfn++) {
        for (ofm1 = my_in_start; ofm1 < my_in_end; ++ofm1) {
          /* -> &LIBXSMM_VLA_ACCESS(4, doutput, bfn*blocks, ofm1, 0, 0, nBlocksOFm, bn, bk) */
          float *l_buf = (float*) doutput + bfn * blocks * N_shift +  ofm1 * OFM_shift;
          my_init_buf(l_buf, OFM_shift, 0, 0);
        }
      }
    }
  } else {
    if (BF == 1) {
      for ( ifm1ofm1 = thr_begin; ifm1ofm1 < thr_end; ++ifm1ofm1 ) {
        /* -> &LIBXSMM_VLA_ACCESS(4, doutput, 0, ofm1, 0, ofm2*bbk, nBlocksOFm, bn, bk) */
        ofm1 = ifm1ofm1 / Cck_work;
        ofm2 = (ifm1ofm1 % Cck_work) / Cc_work;
        float *l_buf = (float*) doutput + ofm1 * OFM_shift + ofm2*bbk;
        my_init_buf(l_buf, bbk * bk, 0, 0);
      }
    } else {
      for (bfn = 0; bfn < BF; bfn++) {
        for ( ifm1ofm1 = thr_begin; ifm1ofm1 < thr_end; ++ifm1ofm1 ) {
          /* -> &LIBXSMM_VLA_ACCESS(4, doutput, bfn*blocks, ofm1, 0, ofm2*bbk, nBlocksOFm, bn, bk), */
          ofm1 = ifm1ofm1 / Cck_work;
          ofm2 = (ifm1ofm1 % Cck_work) / Cc_work;
          float *l_buf = (float*) doutput + bfn * blocks * N_shift +  ofm1 * OFM_shift + ofm2*bbk;
          my_init_buf(l_buf, bbk * bk, 0, 0);
        }
      }
    }
  }
  libxsmm_barrier_wait(cfg.barrier, ltid);

}

int main(int argc, char* argv[])
{
  float **act_libxsmm, **fil_libxsmm, **delact_libxsmm, **delfil_libxsmm;
  float **bias_libxsmm, **delbias_libxsmm;
  unsigned char **relumask_libxsmm;
  int *label_libxsmm;
  my_eltwise_fuse my_fuse;
  my_fc_fwd_config* my_fc_fwd;
  my_fc_bwd_config* my_fc_bwd;
  my_opt_config* my_opt;
  my_smax_fwd_config my_smax_fwd;
  my_smax_bwd_config my_smax_bwd;
  void* scratch = NULL;
  size_t scratch_size = 0;

  /* some parameters we can overwrite via cli,
     default is some inner layer of overfeat */
  int iters = 10;         /* repetitions of benchmark */
  int MB = 256;          /* mini-batch size, "N" */
  int fuse_type = 0;      /* 0: nothing fused, 1: relu fused, 2: elementwise fused, 3: relu and elementwise fused */
  char type = 'A';        /* 'A': ALL, 'F': FP, 'B': BP, 'U', WU */
  int bn = 32;
  int bk = 32;
  int bc = 32;
  int *C;               /* number of input feature maps, "C" */
  int num_layers = 0;

#if defined(_OPENMP)
  int nThreads = omp_get_max_threads(); /* number of threads */
#else
  int nThreads = 1; /* number of threads */
#endif

  unsigned long long l_start, l_end;
  double l_total = 0.0;
  double gflop = 0.0;
  int i, j;
  double fil_size = 0.0;
  double act_size = 0.0;
  float lr = 0.2f;
  float loss = 0;
  float loss_weight = 0.1f;

  libxsmm_matdiff_info norms_fwd, norms_bwd, norms_upd, diff;
  libxsmm_matdiff_clear(&norms_fwd);
  libxsmm_matdiff_clear(&norms_bwd);
  libxsmm_matdiff_clear(&norms_upd);
  libxsmm_matdiff_clear(&diff);

  if (argc > 1 && !strncmp(argv[1], "-h", 3)) {
    printf("Usage: %s iters MB fuse_type type bn bk bc C1 C2 ... CN\n", argv[0]);
    return 0;
  }
  libxsmm_rng_set_seed(1);

  /* reading new values from cli */
  i = 1;
  num_layers = argc - 9;
  if (argc > i) iters      = atoi(argv[i++]);
  if (argc > i) MB         = atoi(argv[i++]);
  if (argc > i) fuse_type  = atoi(argv[i++]);
  if (argc > i) type       = *(argv[i++]);
  if (argc > i) bn         = atoi(argv[i++]);
  if (argc > i) bk         = atoi(argv[i++]);
  if (argc > i) bc         = atoi(argv[i++]);
  /* allocate the number of channles buffer */
  if ( num_layers < 1 ) {
    printf("Usage: %s iters MB fuse_type type bn bk bc C1 C2 ... CN\n", argv[0]);
    return 0;
  }
  C = (int*)malloc((num_layers+2)*sizeof(int));
  for (j = 0 ; i < argc; ++i, ++j ) {
    C[j] = atoi(argv[i]);
  }
  /* handle softmax config */
  C[num_layers+1] = C[num_layers];

  if (type != 'A' && type != 'F' && type != 'B') {
    printf("type needs to be 'A' (All), 'F' (FP only), 'B' (BP only)\n");
    return -1;
  }
  if ( (fuse_type < 0) || (fuse_type > 5) ) {
    printf("fuse type needs to be 0 (None), 1 (Bias), 2 (ReLU), 3 (Sigmoid), 4 (Bias+ReLU), 5 (Bias+Sigmoid)\n");
    return -1;
  }

#if defined(__SSE3__)
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
  _MM_SET_ROUNDING_MODE(_MM_ROUND_NEAREST);
#endif

  /* print some summary */
  printf("##########################################\n");
  printf("#          Setting Up (Common)           #\n");
  printf("##########################################\n");
  printf("PARAMS: N:%d\n", MB);
  printf("PARAMS: Layers: %d\n", num_layers);
  printf("PARAMS: ITERS:%d", iters); printf("  Threads:%d\n", nThreads);
  for (i = 0; i < num_layers; ++i ) {
    if (i == 0) {
      act_size += (double)(MB*C[i]*sizeof(float))/(1024.0*1024.0);
      printf("SIZE Activations  %i (%dx%d): %10.2f MiB\n", i, MB, C[i], (double)(MB*C[i]*sizeof(float))/(1024.0*1024.0) );
    }
    act_size += (double)(MB*C[i+1]*sizeof(float))/(1024.0*1024.0);
    fil_size += (double)(C[i]*C[i+1]*sizeof(float))/(1024.0*1024.0);
    printf("SIZE Filter       %i (%dx%d): %10.2f MiB\n", i, C[i], C[i+1], (double)(C[i]*C[i+1]*sizeof(float))/(1024.0*1024.0) );
    printf("SIZE Activations  %i (%dx%d): %10.2f MiB\n", i+1, MB, C[i+1], (double)(MB*C[i+1]*sizeof(float))/(1024.0*1024.0) );
  }
  act_size += (double)(MB*C[num_layers+1]*sizeof(float))/(1024.0*1024.0);
  printf("SIZE Activations softmax (%dx%d): %10.2f MiB\n", MB, C[num_layers+1], (double)(MB*C[num_layers+1]*sizeof(float))/(1024.0*1024.0) );
  printf("\nTOTAL SIZE Activations:    %10.2f MiB\n", act_size );
  printf("TOTAL SIZE Filter:         %10.2f MiB\n", fil_size );
  printf("TOTAL SIZE delActivations: %10.2f MiB\n", act_size );
  printf("TOTAL SIZE delFilter:      %10.2f MiB\n", fil_size );
  printf("TOTAL SIZE MLP:            %10.2f MiB\n", (2.0*fil_size) + (2.0*act_size) );

  /* allocate data */
  /* +2 because of the softwax layer */
  act_libxsmm    = (float**)malloc( (num_layers+2)*sizeof(float*) );
  delact_libxsmm = (float**)malloc( (num_layers+1)*sizeof(float*) );
  for ( i = 0 ; i < num_layers+2; ++i ) {
    act_libxsmm[i]                = (float*)libxsmm_aligned_malloc( MB*C[i]*sizeof(float), 2097152);
    /* softmax has no incoming gradients */
    if ( i < num_layers+1 ) {
      delact_libxsmm[i]             = (float*)libxsmm_aligned_malloc( MB*C[i]*sizeof(float), 2097152);
    }
  }
  fil_libxsmm    = (float**)malloc( num_layers*sizeof(float*) );
  delfil_libxsmm = (float**)malloc( num_layers*sizeof(float*) );
  for ( i = 0 ; i < num_layers; ++i ) {
    fil_libxsmm[i]                = (float*)libxsmm_aligned_malloc( C[i]*C[i+1]*sizeof(float), 2097152);
    delfil_libxsmm[i]             = (float*)libxsmm_aligned_malloc( C[i]*C[i+1]*sizeof(float), 2097152);
  }
  bias_libxsmm    = (float**)malloc( num_layers*sizeof(float*) );
  delbias_libxsmm = (float**)malloc( num_layers*sizeof(float*) );
  for ( i = 0 ; i < num_layers; ++i ) {
    bias_libxsmm[i]               = (float*)libxsmm_aligned_malloc( C[i+1]*sizeof(float), 2097152);
    delbias_libxsmm[i]            = (float*)libxsmm_aligned_malloc( C[i+1]*sizeof(float), 2097152);
  }
  relumask_libxsmm = (unsigned char**)malloc( num_layers*sizeof(unsigned char*) );
  for ( i = 0 ; i < num_layers; ++i ) {
    relumask_libxsmm[i]           = (unsigned char*)libxsmm_aligned_malloc( MB*C[i+1]*sizeof(unsigned char), 2097152);
  }
  label_libxsmm = (int*)libxsmm_aligned_malloc( MB*sizeof(int), 2097152);

  printf("\n");
  printf("##########################################\n");
  printf("#      Setting Up  (custom-Storage)      #\n");
  printf("##########################################\n");

  if ( fuse_type == 0 ) {
    my_fuse = MY_ELTWISE_FUSE_NONE;
  } else if ( fuse_type == 1 ) {
    my_fuse = MY_ELTWISE_FUSE_BIAS;
  } else if ( fuse_type == 2 ) {
    my_fuse = MY_ELTWISE_FUSE_RELU;
  } else if ( fuse_type == 4 ) {
    my_fuse = MY_ELTWISE_FUSE_BIAS_RELU;
  } else {
    /* cannot happen */
  }

  /* allocating handles */
  my_fc_fwd = (my_fc_fwd_config*) malloc( num_layers*sizeof(my_fc_fwd_config) );
  my_fc_bwd = (my_fc_bwd_config*) malloc( num_layers*sizeof(my_fc_bwd_config) );
  my_opt    = (my_opt_config*)    malloc( num_layers*sizeof(my_opt_config)    );

  int max_bwd_layer = 0;
  size_t max_bwd_scratch_size = 0;
  scratch_size = 0;
  /* setting up handles + scratch */
  for ( i = 0; i < num_layers; ++i ) {
    my_fc_fwd[i] = setup_my_fc_fwd(MB, C[i], C[i+1], (MB % bn == 0) ? bn : MB,
                                             (C[i  ] % bc == 0) ? bc : C[i  ],
                                             (C[i+1] % bk == 0) ? bk : C[i+1],
                                             nThreads, my_fuse);

    my_fc_bwd[i] = setup_my_fc_bwd(MB, C[i], C[i+1], (MB % bn == 0) ? bn : MB,
                                             (C[i  ] % bc == 0) ? bc : C[i  ],
                                             (C[i+1] % bk == 0) ? bk : C[i+1],
                                              nThreads, my_fuse);

    my_opt[i] = setup_my_opt( C[i], C[i+1], (C[i  ] % bc == 0) ? bc : C[i  ],
                                            (C[i+1] % bk == 0) ? bk : C[i+1],
                                            nThreads, lr );
    if (my_fc_bwd[i].scratch_size > 0 && my_fc_bwd[i].scratch_size > max_bwd_scratch_size) {
        max_bwd_layer = i;
        max_bwd_scratch_size =  my_fc_bwd[i].scratch_size;
    }
    /* let's allocate and bind scratch */
    if ( my_fc_fwd[i].scratch_size > 0 || my_fc_bwd[i].scratch_size > 0 || my_opt[i].scratch_size > 0 ) {
      size_t alloc_size = LIBXSMM_MAX( LIBXSMM_MAX( my_fc_fwd[i].scratch_size, my_fc_bwd[i].scratch_size), my_opt[i].scratch_size );
      if ( alloc_size > scratch_size )
        scratch_size = alloc_size;
    }
  }

  /* softmax+loss is treated as N+! layer */
  my_smax_fwd = setup_my_smax_fwd( MB, C[num_layers+1], (MB % bn == 0) ? bn : MB,
                                       (C[num_layers+1] % bk == 0) ? bk : C[num_layers+1],
                                       nThreads );

  my_smax_bwd = setup_my_smax_bwd( MB, C[num_layers+1], (MB % bn == 0) ? bn : MB,
                                       (C[num_layers+1] % bk == 0) ? bk : C[num_layers+1],
                                       nThreads, loss_weight );

  if ( my_smax_fwd.scratch_size > 0 || my_smax_bwd.scratch_size > 0 ) {
    size_t alloc_size = LIBXSMM_MAX( my_smax_fwd.scratch_size, my_smax_bwd.scratch_size );
    if ( alloc_size > scratch_size ) {
      if ( scratch != NULL ) libxsmm_free( scratch );
        scratch_size = alloc_size;
    }
  }
  scratch = libxsmm_aligned_scratch( scratch_size, 2097152 );

  /* init data */
  for ( i = 0 ; i < num_layers+2; ++i ) {
    my_init_buf( act_libxsmm[i], MB*C[i], 0, 0 );
  }
  for ( i = 0 ; i < num_layers; ++i ) {
    my_init_buf( delfil_libxsmm[i], C[i]*C[i+1], 0, 0 );
  }
  for ( i = 0 ; i < num_layers; ++i ) {
    my_init_buf( bias_libxsmm[i], C[i+1], 0, 0 );
  }
  for ( i = 0 ; i < num_layers; ++i ) {
    my_init_buf( delbias_libxsmm[i], C[i+1], 0, 0 );
  }
  for ( i = 0 ; i < num_layers; ++i ) {
    zero_buf_uint8( relumask_libxsmm[i], MB*C[i+1] );
  }
  zero_buf_int32( label_libxsmm, MB );

#if defined(_OPENMP)
# pragma omp parallel
#endif
  {
#if defined(_OPENMP)
    const int tid = omp_get_thread_num();
#else
    const int tid = 0;
#endif
    int l = 0;
    for ( l = 0; l < num_layers; ++l ) {
      init_on_numa_node_fwd( my_fc_fwd[l], fil_libxsmm[l], 0, tid );
    }
    init_on_numa_node_bwd_d( my_fc_bwd[max_bwd_layer], scratch, 0, tid );

    if (!((my_fc_bwd[l].fuse_type & MY_ELTWISE_FUSE_RELU) == MY_ELTWISE_FUSE_RELU))
        for ( l = num_layers-1; l >= 0; --l) {
          init_on_numa_node_bwd_w( my_fc_bwd[l], delact_libxsmm[l+1], 0, tid );
        }
    else {
        init_on_numa_node_bwd_w( my_fc_bwd[max_bwd_layer], scratch + my_fc_bwd[max_bwd_layer].C * my_fc_bwd[max_bwd_layer].K, 0, tid );
    }
  }

  if ( type == 'F') {
    printf("##########################################\n");
    printf("#   Performance - FWD (custom-Storage)   #\n");
    printf("##########################################\n");
    l_start = libxsmm_timer_tick();
#if defined(_OPENMP)
#   pragma omp parallel private(i,j)
#endif
    {
#if defined(_OPENMP)
      const int tid = omp_get_thread_num();
#else
      const int tid = 0;
#endif
      for (j = 0; j < iters; ++j) {
        for ( i = 0; i < num_layers; ++i) {
          my_fc_fwd_exec( my_fc_fwd[i], fil_libxsmm[i], act_libxsmm[i], act_libxsmm[i+1],
                          bias_libxsmm[i], relumask_libxsmm[i], 0, tid, scratch );
        }
#ifdef USE_SOFTMAX
        my_smax_fwd_exec( my_smax_fwd, act_libxsmm[num_layers], act_libxsmm[num_layers+1], label_libxsmm, &loss,
                          0, tid, scratch );
#endif
      }
    }
    l_end = libxsmm_timer_tick();
    l_total = libxsmm_timer_duration(l_start, l_end);

    gflop = 0.0;
    for ( i = 0; i < num_layers; ++i) {
      gflop += (2.0*(double)MB*(double)C[i]*(double)C[i+1]*(double)iters) / (1000.0*1000.0*1000.0);
    }
    printf("GFLOP  = %.5g\n", gflop/(double)iters);
    printf("fp time = %.5g\n", ((double)(l_total/iters)));
    printf("GFLOPS  = %.5g\n", gflop/l_total);
    printf("PERFDUMP,FP,%s,%i,%i,", LIBXSMM_VERSION, nThreads, MB );
    for ( i = 0; i < num_layers; ++i ) {
      printf("%i,", C[i] );
    }
    printf("%f,%f\n", ((double)(l_total/iters)), gflop/l_total);
    /* Print some norms on last act for fwd and weights of first layer after all iterations */
    libxsmm_matdiff(&norms_fwd, LIBXSMM_DATATYPE_F32, MB*C[num_layers], 1, act_libxsmm[num_layers], act_libxsmm[num_layers], 0, 0);
    printf("L1 of act[num_layers]  : %.25g\n", norms_fwd.l1_ref);
  }

  if (type == 'B') {
    printf("##########################################\n");
    printf("#   Performance - BWD (custom-Storage)   #\n");
    printf("##########################################\n");
    l_start = libxsmm_timer_tick();
#if defined(_OPENMP)
#   pragma omp parallel private(i,j)
#endif
    {
#if defined(_OPENMP)
      const int tid = omp_get_thread_num();
#else
      const int tid = 0;
#endif
      for (j = 0; j < iters; ++j) {
#ifdef USE_SOFTMAX
        my_smax_bwd_exec( my_smax_bwd, delact_libxsmm[num_layers], act_libxsmm[num_layers+1], label_libxsmm,
                          0, tid, scratch );
#endif
        for ( i = num_layers-1; i > 0; --i) {
          my_fc_bwd_exec( my_fc_bwd[i], fil_libxsmm[i], delact_libxsmm[i], delact_libxsmm[i+1], delfil_libxsmm[i],
                          act_libxsmm[i], delbias_libxsmm[i], relumask_libxsmm[i], MY_PASS_BWD, 0, tid, scratch );
          my_opt_exec( my_opt[i], fil_libxsmm[i], delfil_libxsmm[i], 0, tid, scratch );
        }
        my_fc_bwd_exec( my_fc_bwd[0], fil_libxsmm[0], delact_libxsmm[0], delact_libxsmm[0+1], delfil_libxsmm[0],
                        act_libxsmm[0], delbias_libxsmm[0], relumask_libxsmm[0], MY_PASS_BWD_W, 0, tid, scratch );
        my_opt_exec( my_opt[0], fil_libxsmm[0], delfil_libxsmm[0], 0, tid, scratch );
      }
    }
    l_end = libxsmm_timer_tick();
    l_total = libxsmm_timer_duration(l_start, l_end);

    gflop = 0.0;
    for ( i = num_layers-1; i > 0; --i) {
      gflop += (4.0*(double)MB*(double)C[i]*(double)C[i+1]*(double)iters) / (1000.0*1000.0*1000.0);
    }
    gflop += (2.0*(double)MB*(double)C[0]*(double)C[1]*(double)iters) / (1000.0*1000.0*1000.0);
    printf("GFLOP  = %.5g\n", gflop/(double)iters);
    printf("fp time = %.5g\n", ((double)(l_total/iters)));
    printf("GFLOPS  = %.5g\n", gflop/l_total);
    printf("PERFDUMP,BP,%s,%i,%i,", LIBXSMM_VERSION, nThreads, MB );
    for ( i = 0; i < num_layers; ++i ) {
      printf("%i,", C[i] );
    }
    printf("%f,%f\n", ((double)(l_total/iters)), gflop/l_total);
  }

  if (type == 'A') {
    printf("##########################################\n");
    printf("# Performance - FWD-BWD (custom-Storage) #\n");
    printf("##########################################\n");
    l_start = libxsmm_timer_tick();
#if defined(_OPENMP)
#   pragma omp parallel private(i,j)
#endif
    {
#if defined(_OPENMP)
      const int tid = omp_get_thread_num();
#else
      const int tid = 0;
#endif
      for (j = 0; j < iters; ++j) {
        for ( i = 0; i < num_layers; ++i) {
          my_fc_fwd_exec( my_fc_fwd[i], fil_libxsmm[i], act_libxsmm[i], act_libxsmm[i+1],
                          bias_libxsmm[i], relumask_libxsmm[i], 0, tid, scratch );
        }
#ifdef USE_SOFTMAX
        my_smax_fwd_exec( my_smax_fwd, act_libxsmm[num_layers], act_libxsmm[num_layers+1], label_libxsmm, &loss,
                          0, tid, scratch );
        my_smax_bwd_exec( my_smax_bwd, delact_libxsmm[num_layers], act_libxsmm[num_layers+1], label_libxsmm,
                          0, tid, scratch );
#endif
        for ( i = num_layers-1; i > 0; --i) {
          my_fc_bwd_exec( my_fc_bwd[i], fil_libxsmm[i], delact_libxsmm[i], delact_libxsmm[i+1], delfil_libxsmm[i],
                          act_libxsmm[i], delbias_libxsmm[i], relumask_libxsmm[i], MY_PASS_BWD, 0, tid, scratch );
          my_opt_exec( my_opt[i], fil_libxsmm[i], delfil_libxsmm[i], 0, tid, scratch );
        }
        my_fc_bwd_exec( my_fc_bwd[0], fil_libxsmm[0], delact_libxsmm[0], delact_libxsmm[0+1], delfil_libxsmm[0],
                        act_libxsmm[0], delbias_libxsmm[0], relumask_libxsmm[0], MY_PASS_BWD_W, 0, tid, scratch );
        my_opt_exec( my_opt[0], fil_libxsmm[0], delfil_libxsmm[0], 0, tid, scratch );
      }
    }
    l_end = libxsmm_timer_tick();
    l_total = libxsmm_timer_duration(l_start, l_end);

#ifdef CHECK_L1
#if 1
    /* Print some norms on last act for fwd and weights of first layer after all iterations */
    libxsmm_matdiff(&norms_fwd, LIBXSMM_DATATYPE_F32, MB*C[num_layers], 1, act_libxsmm[num_layers], act_libxsmm[num_layers], 0, 0);
    printf("L1 of act[num_layers]  : %.25g\n", norms_fwd.l1_ref);
    libxsmm_matdiff_reduce(&diff, &norms_fwd);
    libxsmm_matdiff(&norms_bwd, LIBXSMM_DATATYPE_F32, C[0]*C[1], 1, fil_libxsmm[0], fil_libxsmm[0], 0, 0);
    printf("L1 of wt[0]  : %.25g\n", norms_bwd.l1_ref);
    libxsmm_matdiff_reduce(&diff, &norms_bwd);
#else
    {
      int e = 0;
      FILE *fileAct, *fileWt;
      fileAct = fopen("acts.txt","w+");
      if (fileAct != NULL) {
        for (e = 0; e < MB*C[num_layers]; e++) {
          fprintf(fileAct, "%.10g\n", *((float*)act_libxsmm[num_layers] + e));
        }
        fclose(fileAct);
      }
      fileWt = fopen("weights.txt","w+");
      if (fileWt != NULL) {
        for (e = 0; e < C[0]*C[1]; e++) {
          fprintf(fileWt, "%.10g\n", *((float*)fil_libxsmm[0] + e));
        }
        fclose(fileWt);
      }
    }
#endif
#endif

    gflop = 0.0;
    for ( i = num_layers-1; i > 0; --i) {
      gflop += (6.0*(double)MB*(double)C[i]*(double)C[i+1]*(double)iters) / (1000.0*1000.0*1000.0);
    }
    gflop += (4.0*(double)MB*(double)C[0]*(double)C[1]*(double)iters) / (1000.0*1000.0*1000.0);
    printf("GFLOP  = %.5g\n", gflop/(double)iters);
    printf("fp time = %.5g\n", ((double)(l_total/iters)));
    printf("GFLOPS  = %.5g\n", gflop/l_total);
    printf("PERFDUMP,BP,%s,%i,%i,", LIBXSMM_VERSION, nThreads, MB );
    for ( i = 0; i < num_layers; ++i ) {
      printf("%i,", C[i] );
    }
    printf("%f,%f\n", ((double)(l_total/iters)), gflop/l_total);
  }

  /* deallocate data */
  if ( scratch != NULL ) {
    libxsmm_free(scratch);
  }
  for ( i = 0; i < num_layers; ++i ) {
    if ( i == 0 ) {
      libxsmm_free(act_libxsmm[i]);
      libxsmm_free(delact_libxsmm[i]);
    }
    libxsmm_free(act_libxsmm[i+1]);
    libxsmm_free(delact_libxsmm[i+1]);

    libxsmm_free(fil_libxsmm[i]);
    libxsmm_free(delfil_libxsmm[i]);
    libxsmm_free(bias_libxsmm[i]);
    libxsmm_free(delbias_libxsmm[i]);
    libxsmm_free(relumask_libxsmm[i]);
  }
  libxsmm_free(act_libxsmm[num_layers+1]);
  libxsmm_free(label_libxsmm);

  free( my_opt );
  free( my_fc_fwd );
  free( my_fc_bwd );

  free( act_libxsmm );
  free( delact_libxsmm );
  free( fil_libxsmm );
  free( delfil_libxsmm );
  free( bias_libxsmm );
  free( delbias_libxsmm );
  free( relumask_libxsmm );

  free( C );

  /* some empty lines at the end */
  printf("\n\n\n");

  return 0;
}

