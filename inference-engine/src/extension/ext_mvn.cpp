// Copyright (C) 2018 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0
//

#include "ext_list.hpp"
#include "ext_base.hpp"

#include <cmath>
#include <string>
#include <vector>
#include <cassert>
#include <algorithm>
#if defined(HAVE_AVX2) || defined(HAVE_AVX512F)
#include <immintrin.h>
#endif
#include "ie_parallel.hpp"

namespace InferenceEngine {
namespace Extensions {
namespace Cpu {

inline int div_up(const int a, const int b) {
    assert(b);
    return (a + b - 1) / b;
}

class MVNImpl: public ExtLayerBase {
public:
    explicit MVNImpl(const CNNLayer* layer) {
        try {
            if (layer->insData.size() != 1 || layer->outData.empty())
                THROW_IE_EXCEPTION << "Incorrect number of input/output edges!";

            across_channels = static_cast<bool>(layer->GetParamAsInt("across_channels"));
            normalize_variance = static_cast<bool>(layer->GetParamAsInt("normalize_variance"));
            eps = layer->GetParamAsFloat("eps");

#if defined(HAVE_AVX512F)
            auto blk_layout = ConfLayout::BLK16;
#else
            auto blk_layout = ConfLayout::BLK8;
#endif
            addConfig(layer, {{blk_layout, false, -1}}, {{blk_layout, false, 0}});
            addConfig(layer, {{ConfLayout::PLN, false, 0}}, {{ConfLayout::PLN, false, 0}});
        } catch (InferenceEngine::details::InferenceEngineException &ex) {
            errorMsg = ex.what();
        }
    }

    StatusCode execute(std::vector<Blob::Ptr>& inputs, std::vector<Blob::Ptr>& outputs,
                       ResponseDesc *resp) noexcept override {
        float* src_data = inputs[0]->buffer();
        float* dst_data = outputs[0]->buffer();

        SizeVector dims = inputs[0]->getTensorDesc().getDims();

        int N = static_cast<int>((dims.size() > 0) ? dims[0] : 1);
        int C = static_cast<int>((dims.size() > 1) ? dims[1] : 1);
        int H = static_cast<int>((dims.size() > 2) ? dims[2] : 1);
        int W = static_cast<int>((dims.size() > 3) ? dims[3] : 1);

        if (inputs[0]->layout() == NCHW) {
            mvn_pln(src_data, dst_data, N, C, H, W);
        } else {
            mvn_blk(src_data, dst_data, N, C, H, W);
        }

        return OK;
    }

private:
    void mvn_pln(const float* src_data, float* dst_data, int N, int C, int H, int W);
    void mvn_blk(const float* src_data, float* dst_data, int N, int C, int H, int W);

    bool across_channels = false;
    bool normalize_variance = true;
    float eps = 1e-9f;
};

void MVNImpl::mvn_pln(const float* src_data, float* dst_data, int N, int C, int H, int W) {
    for (int b = 0; b < N; b++) {
        // Calculate mean value
        if (across_channels) {
            double mean = 0.0;
            mean = parallel_sum(C, mean, [&](int c)->double {
                double mean_internal = 0.0;
                for (int h = 0; h < H; h++) {
                    for (int w = 0; w < W; w++) {
                        mean_internal += src_data[b*C*H*W + c*H*W + h*W + w];
                    }
                }
                return mean_internal;
            });

            mean /= C*H*W;
            parallel_for(C, [&](int c) {
                for (int h = 0; h < H; h++) {
                    for (int w = 0; w < W; w++) {
                        dst_data[b*C*H*W + c*H*W + h*W + w] = src_data[b*C*H*W + c*H*W + h*W + w] - mean;
                    }
                }
            });
        } else {
            parallel_for(C, [&](int c) {
                double mean = 0;
                for (int h = 0; h < H; h++) {
                    for (int w = 0; w < W; w++) {
                        mean += src_data[b*C*H*W + c*H*W + h*W + w];
                    }
                }
                mean /= H*W;

                for (int h = 0; h < H; h++) {
                    for (int w = 0; w < W; w++) {
                        dst_data[b*C*H*W + c*H*W + h*W + w] = src_data[b*C*H*W + c*H*W + h*W + w] - mean;
                    }
                }
            });
        }
    }

    if (normalize_variance) {
        for (int b = 0; b < N; b++) {
            // Calculate variances value
            if (across_channels) {
                double variance = 0.0;
                variance = parallel_sum(C, variance, [&](int c)->double {
                    double variance_internal = 0.0;
                    for (int h = 0; h < H; h++) {
                        for (int w = 0; w < W; w++) {
                            variance_internal += std::pow(dst_data[b*C*H*W + c*H*W + h*W + w], 2);
                        }
                    }
                    return variance_internal;
                });

                variance /= C*H*W;
                variance = std::pow(variance, 0.5f);
                variance += eps;
                parallel_for(C, [&](int c) {
                    for (int h = 0; h < H; h++) {
                        for (int w = 0; w < W; w++) {
                            dst_data[b*C*H*W + c*H*W + h*W + w] /= variance;
                        }
                    }
                });
            } else {
                parallel_for(C, [&](int c) {
                    double variance = 0;
                    for (int h = 0; h < H; h++) {
                        for (int w = 0; w < W; w++) {
                            variance += std::pow(dst_data[b*C*H*W + c*H*W + h*W + w], 2);
                        }
                    }
                    variance /= H*W;
                    variance = std::pow(variance, 0.5f);
                    variance += eps;
                    for (int h = 0; h < H; h++) {
                        for (int w = 0; w < W; w++) {
                            dst_data[b*C*H*W + c*H*W + h*W + w] /= variance;
                        }
                    }
                });
            }
        }
    }
}

void MVNImpl::mvn_blk(const float* src_data, float* dst_data, int N, int C, int H, int W) {
#if defined(HAVE_AVX512F)
    size_t blk_size = 16;
#else
    size_t blk_size = 8;
#endif

#if defined(HAVE_AVX512F)
    typedef __m512 vec_type;
#elif defined(HAVE_AVX2)
    typedef __m256 vec_type;
#endif

    int CB = div_up(C, static_cast<int>(blk_size));

    if (normalize_variance) {
        for (int b = 0; b < N; b++) {
            if (across_channels) {
                float mean = 0.0f;
                mean = parallel_sum2d(CB, H, mean, [&](int cb, int h)->float {
                    float mean_internal = 0.0;
                    for (int w = 0; w < W; w++) {
                        for (int c = 0; c < std::min(blk_size, C - cb * blk_size); c++) {
                            size_t src_offset = b*CB*H*W*blk_size + cb*H*W*blk_size + h*W*blk_size + w*blk_size + c;

                            mean_internal += src_data[src_offset];
                        }
                    }
                    return mean_internal;
                });

                mean /= C * H * W;

                float variance = 0.0f;
                variance = parallel_sum2d(CB, H, variance, [&](int cb, int h)->float {
                    float variance_internal = 0.0;
                    for (int w = 0; w < W; w++) {
                        for (int c = 0; c < std::min(blk_size, C - cb * blk_size); c++) {
                            size_t src_offset = b*CB*H*W*blk_size + cb*H*W*blk_size + h*W*blk_size + w*blk_size + c;

                            variance_internal += std::pow(src_data[src_offset] - mean, 2);
                        }
                    }
                    return variance_internal;
                });

                variance /= C*H*W;
                variance = std::pow(variance, 0.5f);
                variance += eps;

                parallel_for2d(CB, H, [&](int cb, int h) {
                    for (int w = 0; w < W; w++) {
                        for (int c = 0; c < std::min(blk_size, C - cb * blk_size); c++) {
                            size_t src_offset = b*CB*H*W*blk_size + cb*H*W*blk_size + h*W*blk_size + w*blk_size + c;

                            dst_data[src_offset] = (src_data[src_offset] - mean) / variance;
                        }
                    }
                });
            } else {
                parallel_for(CB, [&](int cb) {
                    size_t src_off = b*CB*H*W*blk_size + cb*H*W*blk_size;
#if defined(HAVE_AVX2) || defined(HAVE_AVX512F)
                    vec_type vmean = _mm_uni_setzero_ps();
                    for (int h = 0; h < H; h++) {
                        for (int w = 0; w < W; w++) {
                            vec_type vsrc = _mm_uni_loadu_ps(src_data + src_off + h*W*blk_size + w*blk_size);
                            vmean = _mm_uni_add_ps(vmean, vsrc);
                        }
                    }

                    vec_type vsize = _mm_uni_set1_ps(static_cast<float>(H * W));
                    vmean = _mm_uni_div_ps(vmean, vsize);

                    vec_type vvariance = _mm_uni_setzero_ps();
                    for (int h = 0; h < H; h++) {
                        for (int w = 0; w < W; w++) {
                            vec_type vsrc = _mm_uni_loadu_ps(src_data + src_off + h*W*blk_size + w*blk_size);
                            vsrc = _mm_uni_sub_ps(vsrc, vmean);
                            vvariance = _mm_uni_add_ps(vvariance, _mm_uni_mul_ps(vsrc, vsrc));
                        }
                    }

                    vvariance = _mm_uni_div_ps(vvariance, vsize);
                    vvariance = _mm_uni_sqrt_ps(vvariance);

                    vec_type veps = _mm_uni_set1_ps(eps);
                    vvariance = _mm_uni_add_ps(vvariance, veps);

                    for (int h = 0; h < H; h++) {
                        for (int w = 0; w < W; w++) {
                            vec_type vsrc = _mm_uni_loadu_ps(src_data + src_off + h*W*blk_size + w*blk_size);
                            vsrc = _mm_uni_sub_ps(vsrc, vmean);
                            _mm_uni_storeu_ps(dst_data + src_off + h*W*blk_size + w*blk_size, _mm_uni_div_ps(vsrc, vvariance));
                        }
                    }
#else
                    for (int c = 0; c < std::min(blk_size, C - cb * blk_size); c++) {
                        float mean = 0;
                        for (int h = 0; h < H; h++) {
                            for (int w = 0; w < W; w++) {
                                mean += src_data[src_off + h*W*blk_size + w*blk_size + c];
                            }
                        }

                        mean /= H * W;

                        float variance = 0;
                        for (int h = 0; h < H; h++) {
                            for (int w = 0; w < W; w++) {
                                float value = src_data[src_off + h*W*blk_size + w*blk_size + c] - mean;
                                variance += std::pow(value, 2);
                            }
                        }

                        variance /= H * W;
                        variance = std::pow(variance, 0.5f);
                        variance += eps;

                        for (int h = 0; h < H; h++) {
                            for (int w = 0; w < W; w++) {
                                dst_data[src_off + h*W*blk_size + w*blk_size + c] = (src_data[src_off + h*W*blk_size + w*blk_size + c] - mean) / variance;
                            }
                        }
                    }
#endif
                });
            }
        }
    } else {
        for (int b = 0; b < N; b++) {
            if (across_channels) {
                float mean = 0.0f;
                mean = parallel_sum2d(CB, H, mean, [&](int cb, int h)->float {
                    float mean_internal = 0;
                    for (int w = 0; w < W; w++) {
                        for (int c = 0; c < std::min(blk_size, C - cb * blk_size); c++) {
                            size_t src_offset = b*CB*H*W*blk_size + cb*H*W*blk_size + h*W*blk_size + w*blk_size + c;

                            mean_internal += src_data[src_offset];
                        }
                    }
                    return mean_internal;
                });

                mean /= C * H * W;

                parallel_for2d(CB, H, [&](int cb, int h) {
                    for (int w = 0; w < W; w++) {
                        for (int c = 0; c < std::min(blk_size, C - cb * blk_size); c++) {
                            size_t src_offset = b*CB*H*W*blk_size + cb*H*W*blk_size + h*W*blk_size + w*blk_size + c;

                            dst_data[src_offset] = src_data[src_offset] - mean;
                        }
                    }
                });
            } else {
                parallel_for(CB, [&](int cb) {
                    size_t src_off = b*CB*H*W*blk_size + cb*H*W*blk_size;
#if defined(HAVE_AVX2) || defined(HAVE_AVX512F)
                    vec_type vmean = _mm_uni_setzero_ps();
                    for (int h = 0; h < H; h++) {
                        for (int w = 0; w < W; w++) {
                            vec_type vsrc = _mm_uni_loadu_ps(src_data + src_off + h*W*blk_size + w*blk_size);
                            vmean = _mm_uni_add_ps(vmean, vsrc);
                        }
                    }

                    vec_type vsize = _mm_uni_set1_ps(static_cast<float>(H * W));
                    vmean = _mm_uni_div_ps(vmean, vsize);

                    for (int h = 0; h < H; h++) {
                        for (int w = 0; w < W; w++) {
                            vec_type vsrc = _mm_uni_loadu_ps(src_data + src_off + h*W*blk_size + w*blk_size);
                            _mm_uni_storeu_ps(dst_data + src_off + h*W*blk_size + w*blk_size, _mm_uni_sub_ps(vsrc, vmean));
                        }
                    }
#else
                    for (int c = 0; c < std::min(blk_size, C - cb * blk_size); c++) {
                        float mean = 0;
                        for (int h = 0; h < H; h++) {
                            for (int w = 0; w < W; w++) {
                                mean += src_data[src_off + h*W*blk_size + w*blk_size + c];
                            }
                        }

                        mean /= H * W;

                        for (int h = 0; h < H; h++) {
                            for (int w = 0; w < W; w++) {
                                dst_data[src_off + h*W*blk_size + w*blk_size + c] = src_data[src_off + h*W*blk_size + w*blk_size + c] - mean;
                            }
                        }
                    }
#endif
                });
            }
        }
    }
}

REG_FACTORY_FOR(ImplFactory<MVNImpl>, MVN);

}  // namespace Cpu
}  // namespace Extensions
}  // namespace InferenceEngine
