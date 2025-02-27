/*
 Copyright (c) Meta Platforms, Inc. and affiliates.

 This source code is licensed under the BSD3 license found in the
 LICENSE file in the root directory of this source tree.
 */

#include "ComplexityAnalyzer.h"
#include "EOFException.h"

#include "motion_search.h"
#include "frame.h"
#include "moments.h"



ComplexityAnalyzer::ComplexityAnalyzer(IVideoSequenceReader *reader,
                                       int gop_size, int num_frames, int b_frames)
    : m_dim(reader->dim()),
      m_stride(reader->dim().width + 2 * HORIZONTAL_PADDING),
      m_padded_height(reader->dim().height + 2 * VERTICAL_PADDING),
      m_num_frames(num_frames), m_GOP_size(gop_size), m_subGOP_size(b_frames + 1),
      m_pReader(reader), m_pReorderedInfo(NULL) {
  m_GOP_error = 0;
  m_GOP_bits = 0;
  m_GOP_count = 0;

  for (int i = 0; i <= m_subGOP_size; i++)
    pics.push_back(new YUVFrame(m_dim));

  m_pPmv = new MotionVectorField(m_dim, m_stride, m_padded_height, MB_WIDTH);
  m_pB1mv = new MotionVectorField(m_dim, m_stride, m_padded_height, MB_WIDTH);
  m_pB2mv = new MotionVectorField(m_dim, m_stride, m_padded_height, MB_WIDTH);

  int stride_MB = m_dim.width / MB_WIDTH + 2;
  int padded_height_MB = (m_dim.height + MB_WIDTH - 1) / MB_WIDTH + 2;

  const size_t numItems = (size_t)(stride_MB) * (padded_height_MB);
  m_mses = memory::AlignedAlloc<int>(numItems);
  if (m_mses == NULL) {
    fprintf(stderr, "Not enough memory (%zu bytes) for %s\n", numItems * sizeof(int),
           "m_mses");
    exit(-1);
  }
  m_MB_modes = memory::AlignedAlloc<unsigned char>(numItems);
  if (m_MB_modes == NULL) {
    fprintf(stderr, "Not enough memory (%zu bytes) for %s\n",
           numItems * sizeof(unsigned char), "m_MB_modes");
    exit(-1);
  }
}

ComplexityAnalyzer::~ComplexityAnalyzer(void) {
  pics.clear();

  delete m_pPmv;
  delete m_pB1mv;
  delete m_pB2mv;
}

void ComplexityAnalyzer::reset_gop_start(void) {
  m_pPmv->reset();
  m_pB1mv->reset();
  m_pB2mv->reset();
}

void ComplexityAnalyzer::add_info(int num, char p, int err, int count_I,
                                  int count_P, int count_B, int bits) {
  complexity_info_t *i = new complexity_info_t;

  // Convert all numbering to 0..N-1
  i->picNum = num - 1;
  i->picType = p;
  i->error = err;
  i->count_I = count_I;
  i->count_P = count_P;
  i->count_B = count_B;
  i->bits = bits;

  if (p == 'I' || p == 'P') {
    if (m_pReorderedInfo != NULL)
      m_info.push_back(m_pReorderedInfo);
    m_pReorderedInfo = i;
  } else {
    m_info.push_back(i);
  }
}

void ComplexityAnalyzer::process_i_picture(YUVFrame *pict) {
  reset_gop_start();
  int error = m_pPmv->predictSpatial(pict, &m_mses.get()[m_pPmv->firstMB()],
                                     &m_MB_modes.get()[m_pPmv->firstMB()]);
  int bits = m_pPmv->bits();

  // We are weighting I-frames by 10% more bits (282/256), since the QP needs to
  // be the lowest among I/P/B
  bits = (I_FRAME_BIT_WEIGHT * bits + 128) >> 8;
  m_GOP_bits += bits;
  m_GOP_error += error;
  add_info(m_pReader->count(), 'I', error, m_pPmv->count_I(), 0, 0, bits);
  // for debugging
  // fprintf(stderr, "Frame %6d (I), I:%6d, P:%6d, B:%6d, MSE = %9d, bits =
  // %7d\n",pict->pos()+1,m_pPmv->count_I(),0,0,error,bits);
  pict->boundaryExtend();
}

void ComplexityAnalyzer::process_p_picture(YUVFrame *pict, YUVFrame *ref) {
  int error =
      m_pPmv->predictTemporal(pict, ref, &m_mses.get()[m_pPmv->firstMB()],
                              &m_MB_modes.get()[m_pPmv->firstMB()]);
  int bits = m_pPmv->bits();

  // We are weighting P-frames by 5% more bits (269/256), since the QP needs to
  // be lower than B (but higher than I)
  bits = (P_FRAME_BIT_WEIGHT * bits + 128) >> 8;
  m_GOP_bits += bits;
  m_GOP_error += error;
  add_info(m_pReader->count(), 'P', error, m_pPmv->count_I(), m_pPmv->count_P(),
           0, bits);
  // for debugging
  // fprintf(stderr, "Frame %6d (P), I:%6d, P:%6d, B:%6d, MSE = %9d, bits =
  // %7d\n",pict->pos()+1,m_pPmv->count_I(),m_pPmv->count_P(),0,error,bits);
  pict->boundaryExtend();
}

void ComplexityAnalyzer::process_b_picture(YUVFrame *pict, YUVFrame *fwdref,
                                           YUVFrame *backref) {
  int error = m_pPmv->predictBidirectional(
      pict, fwdref, backref, m_pB1mv, m_pB2mv, &m_mses.get()[m_pPmv->firstMB()],
      &m_MB_modes.get()[m_pPmv->firstMB()]);
  int bits = m_pPmv->bits();

  // We are weighting B-frames by 0% more bits (256/256), since QP needs to be
  // highest among I/P/B
  bits = (B_FRAME_BIT_WEIGHT * bits + 128) >> 8;
  m_GOP_bits += bits;
  m_GOP_error += error;
  add_info(m_pReader->count() - (backref->pos() - pict->pos()), 'B', error,
           m_pPmv->count_I(), m_pPmv->count_P(), m_pPmv->count_B(), bits);
  // for debugging
  // fprintf(stderr, "Frame %6d (B), I:%6d, P:%6d, B:%6d, MSE = %9d, bits =
  // %7d\n",pict->pos()+1,m_pPmv->count_I(),m_pPmv->count_P(),m_pPmv->count_B(),error,bits);
}

void ComplexityAnalyzer::analyze() {
  int td = 0;
  int td_ref;

  try {
    while (m_num_frames > 0 ? m_pReader->count() < m_num_frames
                            : !m_pReader->eof()) {
      fprintf(stderr, "Picture count: %d\r", m_pReader->count() - 1);

      if ((m_pReader->count() % m_GOP_size) == 0) {
        if (m_pReader->count()) {
          fprintf(stderr, "GOP: %d, GOP-bits: %d\n", m_GOP_count, m_GOP_bits);
          m_GOP_count++;
        }
        m_GOP_error = 0;
        m_GOP_bits = 0;

        td = 0;
        YUVFrame* pic = pics[0];
        pic->setPos(m_pReader->count());
        m_pReader->read(pic->y(), pic->u(), pic->v());
        process_i_picture(pics[0]);
      } else {
        pics[0]->swapFrame(pics[(size_t)m_subGOP_size]);
      }

      for (td_ref = td; td < (m_GOP_size - 1) && (td - td_ref) < m_subGOP_size;
           td++) {
        YUVFrame* pic = pics[(size_t)(td + 1 - td_ref)];
        pic->setPos(m_pReader->count());
        m_pReader->read(pic->y(), pic->u(), pic->v());
      }

      process_p_picture(/* target    */ pics[(size_t)(td - td_ref)],
                        /* reference */ pics[0]);

      for (int j = 1; j < td - td_ref; j++) {
        process_b_picture(/* target  */ pics[(size_t)j],
                          /* forward */ pics[0],
                          /* reverse */ pics[(size_t)(td - td_ref)]);
      }
    }
  } catch (EOFException &e) {
    fprintf(stderr, "\n%s\n", e.what());
  }

  if (m_pReorderedInfo != NULL)
    m_info.push_back(m_pReorderedInfo);

  fprintf(stderr, "Processed frames: %d\n", m_pReader->count());
}
