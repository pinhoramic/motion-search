/*
 Copyright (c) Meta Platforms, Inc. and affiliates.

 This source code is licensed under the BSD3 license found in the
 LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "common.h"

#include <memory.h>

namespace memory {
struct aligned_deallocator;
template <typename data_t>
using aligned_unique_ptr = std::unique_ptr<data_t, aligned_deallocator>;
} // namespace memory

class YUVFrame {
public:
  YUVFrame(DIM dim);
  virtual ~YUVFrame(void) = default;

  inline uint8_t *y(void) { return m_pY; }
  inline uint8_t *u(void) { return m_pU; }
  inline uint8_t *v(void) { return m_pV; }
  inline int pos(void) { return m_pos; }
  inline const DIM dim(void) { return m_dim; }
  inline int stride(void) { return m_stride; }

  inline void setPos(int pos) { m_pos = pos; }

  void swapFrame(YUVFrame *other);
  void boundaryExtend(void);

private:
  const DIM m_dim;
  const int m_stride = 0;
  const int m_padded_height = 0;

  memory::aligned_unique_ptr<uint8_t> m_pFrame;
  uint8_t *m_pY;
  uint8_t *m_pU;
  uint8_t *m_pV;

  int m_pos;

  YUVFrame(YUVFrame &) = delete;
  YUVFrame &operator=(YUVFrame &) = delete;
};
