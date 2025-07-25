/**
 * Copyright 2019-2025, XGBoost Contributors
 */

#ifndef XGBOOST_DATA_ELLPACK_PAGE_SOURCE_H_
#define XGBOOST_DATA_ELLPACK_PAGE_SOURCE_H_

#include <cstdint>  // for int32_t
#include <limits>   // for numeric_limits
#include <memory>   // for shared_ptr
#include <tuple>    // for tuple
#include <utility>  // for move
#include <vector>   // for vector

#include "../common/compressed_iterator.h"  // for CompressedByteT
#include "../common/cuda_rt_utils.h"        // for SupportsPageableMem, SupportsAts
#include "../common/device_compression.h"   // for SnappyDecomprMgr
#include "../common/hist_util.h"            // for HistogramCuts
#include "../common/ref_resource_view.h"    // for RefResourceView
#include "../data/batch_utils.h"            // for AutoHostRatio
#include "ellpack_page.h"                   // for EllpackPage
#include "ellpack_page_raw_format.h"        // for EllpackPageRawFormat
#include "sparse_page_source.h"             // for PageSourceIncMixIn
#include "xgboost/base.h"                   // for bst_idx_t
#include "xgboost/context.h"                // for DeviceOrd
#include "xgboost/data.h"                   // for BatchParam
#include "xgboost/span.h"                   // for Span

namespace xgboost::curt {
class StreamPool;
}
namespace xgboost::common::cuda_impl {
class HostPinnedMemPool;
}  // namespace xgboost::common::cuda_impl

namespace xgboost::data {
struct EllpackCacheInfo {
  BatchParam param;
  // The size ratio the host cache vs. the total cache
  double cache_host_ratio{::xgboost::cuda_impl::AutoHostRatio()};
  float missing{std::numeric_limits<float>::quiet_NaN()};
  // The ratio of the cache that can be compressed. Used for testing.
  float hw_decomp_ratio{std::numeric_limits<float>::quiet_NaN()};
  bool allow_decomp_fallback{false};
  std::vector<bst_idx_t> cache_mapping;
  std::vector<bst_idx_t> buffer_bytes;  // N bytes of the concatenated pages.
  std::vector<bst_idx_t> buffer_rows;

  EllpackCacheInfo() = default;
  EllpackCacheInfo(BatchParam param, double h_ratio, float missing)
      : param{std::move(param)}, cache_host_ratio{h_ratio}, missing{missing} {}
  EllpackCacheInfo(BatchParam param, ExtMemConfig const& config)
      : param{std::move(param)},
        cache_host_ratio{config.cache_host_ratio},
        missing{config.missing},
        hw_decomp_ratio{config.hw_decomp_ratio},
        allow_decomp_fallback{config.allow_decomp_fallback} {}

  // Only effective for host-based cache.
  // The number of batches for the concatenated cache.
  [[nodiscard]] std::size_t NumBatchesCc() const { return this->buffer_rows.size(); }
};

// We need to decouple the storage and the view of the storage so that we can implement
// concurrent read. As a result, there are two classes, one for cache storage, another one
// for stream.
//
// This is a memory-based cache. It can be a mixed of the device memory and the host
// memory.
struct EllpackMemCache {
  // The host portion of each page.
  std::vector<std::unique_ptr<EllpackPageImpl>> h_pages;
  // The device portion of each page.
  using DPage = common::RefResourceView<common::CompressedByteT>;
  std::vector<DPage> d_pages;
  // Storage for decompression parameters and the compressed buffer.
  using CPage = std::pair<dc::SnappyDecomprMgr, common::RefResourceView<std::uint8_t>>;
  // Compressed host page.
  std::vector<CPage> c_pages;

  using PagePtr = std::tuple<EllpackPageImpl const*, DPage const*, CPage const*>;
  using PageRef = std::tuple<std::unique_ptr<EllpackPageImpl>&, DPage&, CPage&>;

  std::vector<std::size_t> offsets;
  // Size of each batch before concatenation.
  std::vector<bst_idx_t> sizes_orig;
  // Mapping of pages before concatenation to after concatenation.
  std::vector<std::size_t> const cache_mapping;
  // Cache info
  std::vector<std::size_t> const buffer_bytes;
  std::vector<bst_idx_t> const buffer_rows;
  double const cache_host_ratio;
  float const hw_decomp_ratio;
  bool const allow_decomp_fallback;

  std::unique_ptr<curt::StreamPool> streams;  // For decompression
  std::shared_ptr<common::cuda_impl::HostPinnedMemPool> pool;

  explicit EllpackMemCache(EllpackCacheInfo cinfo, std::int32_t n_workers);
  ~EllpackMemCache();

  // The number of bytes of the entire cache.
  [[nodiscard]] std::size_t SizeBytes() const noexcept(true);
  // The number of bytes of the device cache.
  [[nodiscard]] std::size_t DeviceSizeBytes() const noexcept(true);
  // The number of bytes of each page.
  [[nodiscard]] std::size_t SizeBytes(std::size_t i) const noexcept(true);
  // The number of bytes of the gradient index (ellpack).
  [[nodiscard]] std::size_t GidxSizeBytes(std::size_t i) const noexcept(true);
  // The number of bytes of the gradient index (ellpack) of the entire cache.
  [[nodiscard]] std::size_t GidxSizeBytes() const noexcept(true);
  // The number of pages in the cache.
  [[nodiscard]] std::size_t Size() const { return this->h_pages.size(); }
  // Is the cache empty?
  [[nodiscard]] bool Empty() const { return this->SizeBytes() == 0; }
  // No page concatenation is performed. If there's page concatenation, then the number of
  // pages in the cache must be smaller than the input number of pages.
  [[nodiscard]] bool NoConcat() const { return this->NumBatchesOrig() == this->buffer_rows.size(); }
  // The number of pages before concatenatioin.
  [[nodiscard]] bst_idx_t NumBatchesOrig() const { return cache_mapping.size(); }
  // Get the pointers to the k^th concatenated page.
  [[nodiscard]] PagePtr At(std::int32_t k) const;
  // Get a reference to the last concatenated page.
  [[nodiscard]] PageRef Back();
};

// Pimpl to hide CUDA calls from the host compiler.
class EllpackHostCacheStreamImpl;

/**
 * @brief A view of the actual cache implemented by `EllpackHostCache`.
 */
class EllpackHostCacheStream {
  std::unique_ptr<EllpackHostCacheStreamImpl> p_impl_;

 public:
  explicit EllpackHostCacheStream(std::shared_ptr<EllpackMemCache> cache);
  ~EllpackHostCacheStream();
  /**
   * @brief Get a shared handler to the cache.
   */
  std::shared_ptr<EllpackMemCache const> Share() const;
  /**
   * @brief Stream seek.
   *
   * @param offset_bytes This must align to the actual cached page size.
   */
  void Seek(bst_idx_t offset_bytes);
  /**
   * @brief Read a page from the cache.
   *
   * The read page might be concatenated during page write.
   *
   * @param page[out] The returned page.
   * @param prefetch_copy[in] Does the stream need to copy the page?
   */
  void Read(Context const* ctx, EllpackPage* page, bool prefetch_copy) const;
  /**
   * @brief Add a new page to the host cache.
   *
   * This method might append the input page to a previously stored page to increase
   * individual page size.
   *
   * @return Whether a new cache page is create. False if the new page is appended to the
   * previous one.
   */
  [[nodiscard]] bool Write(EllpackPage const& page);
};

namespace detail {
// Not a member of `EllpackFormatPolicy`. Hide the impl without requiring template specialization.
void EllpackFormatCheckNuma(StringView msg);
}  // namespace detail

template <typename S>
class EllpackFormatPolicy {
  std::shared_ptr<common::HistogramCuts const> cuts_{nullptr};
  DeviceOrd device_;
  bool has_hmm_{curt::SupportsPageableMem()};

  EllpackCacheInfo cache_info_;
  static_assert(std::is_same_v<S, EllpackPage>);

 public:
  using FormatT = EllpackPageRawFormat;

 public:
  EllpackFormatPolicy() {
    StringView msg{" The overhead of iterating through external memory might be significant."};
    if (!has_hmm_) {
      LOG(WARNING) << "CUDA heterogeneous memory management is not available." << msg;
    } else if (!curt::SupportsAts()) {
      LOG(WARNING) << "CUDA address translation service is not available." << msg;
    }
#if !defined(XGBOOST_USE_RMM)
    LOG(WARNING) << "XGBoost is not built with RMM support." << msg;
#endif
    if (!GlobalConfigThreadLocalStore::Get()->use_rmm) {
      LOG(WARNING) << "`use_rmm` is set to false." << msg;
    }
    std::int32_t major{0}, minor{0};
    curt::GetDrVersionGlobal(&major, &minor);
    if ((major < 12 || (major == 12 && minor < 7)) && curt::SupportsAts()) {
      // Use ATS, but with an old kernel driver.
      LOG(WARNING) << "Using an old kernel driver with supported CTK<12.7."
                   << "The latest version of CTK supported by the current driver: " << major << "."
                   << minor << "." << msg;
    }
    detail::EllpackFormatCheckNuma(msg);
  }
  // For testing with the HMM flag.
  explicit EllpackFormatPolicy(bool has_hmm) : has_hmm_{has_hmm} {}

  [[nodiscard]] auto CreatePageFormat(BatchParam const& param) const {
    CHECK_EQ(cuts_->cut_values_.Device(), device_);
    std::unique_ptr<FormatT> fmt{new EllpackPageRawFormat{cuts_, device_, param, has_hmm_}};
    return fmt;
  }
  void SetCuts(std::shared_ptr<common::HistogramCuts const> cuts, DeviceOrd device,
               EllpackCacheInfo cinfo) {
    std::swap(this->cuts_, cuts);
    this->device_ = device;
    CHECK(this->device_.IsCUDA());
    this->cache_info_ = std::move(cinfo);
  }
  [[nodiscard]] auto GetCuts() const {
    CHECK(cuts_);
    return cuts_;
  }
  [[nodiscard]] auto Device() const { return this->device_; }
  [[nodiscard]] auto const& CacheInfo() { return this->cache_info_; }
};

template <typename S, template <typename> typename F>
class EllpackCacheStreamPolicy : public F<S> {
  std::shared_ptr<EllpackMemCache> p_cache_;

 public:
  using WriterT = EllpackHostCacheStream;
  using ReaderT = EllpackHostCacheStream;

 public:
  EllpackCacheStreamPolicy() = default;
  [[nodiscard]] std::unique_ptr<WriterT> CreateWriter(StringView name, std::uint32_t iter);

  [[nodiscard]] std::unique_ptr<ReaderT> CreateReader(StringView name, bst_idx_t offset,
                                                      bst_idx_t length) const;
  std::shared_ptr<EllpackMemCache const> Share() const { return p_cache_; }
};

template <typename S, template <typename> typename F>
class EllpackMmapStreamPolicy : public F<S> {
  bool has_hmm_{curt::SupportsPageableMem()};

 public:
  using WriterT = common::AlignedFileWriteStream;
  using ReaderT = common::AlignedResourceReadStream;

 public:
  EllpackMmapStreamPolicy() = default;
  // For testing with the HMM flag.
  template <
      typename std::enable_if_t<std::is_same_v<F<S>, EllpackFormatPolicy<EllpackPage>>>* = nullptr>
  explicit EllpackMmapStreamPolicy(bool has_hmm) : F<S>{has_hmm}, has_hmm_{has_hmm} {}

  [[nodiscard]] std::unique_ptr<WriterT> CreateWriter(StringView name, std::uint32_t iter) {
    std::unique_ptr<common::AlignedFileWriteStream> fo;
    if (iter == 0) {
      fo = std::make_unique<common::AlignedFileWriteStream>(name, "wb");
    } else {
      fo = std::make_unique<common::AlignedFileWriteStream>(name, "ab");
    }
    return fo;
  }

  [[nodiscard]] std::unique_ptr<ReaderT> CreateReader(StringView name, bst_idx_t offset,
                                                      bst_idx_t length) const;
};

/**
 * @brief Calculate the size of each internal cached page along with the mapping of old
 *        pages to the new pages.
 */
void CalcCacheMapping(Context const* ctx, bool is_dense,
                      std::shared_ptr<common::HistogramCuts const> cuts,
                      std::int64_t min_cache_page_bytes, ExternalDataInfo const& ext_info,
                      bool is_validation, EllpackCacheInfo* cinfo);

/**
 * @brief Ellpack source with sparse pages as the underlying source.
 */
template <typename F>
class EllpackPageSourceImpl : public PageSourceIncMixIn<EllpackPage, F> {
  using Super = PageSourceIncMixIn<EllpackPage, F>;
  bool is_dense_;
  bst_idx_t row_stride_;
  BatchParam param_;
  common::Span<FeatureType const> feature_types_;

 public:
  EllpackPageSourceImpl(Context const* ctx, bst_feature_t n_features, std::size_t n_batches,
                        std::shared_ptr<Cache> cache, std::shared_ptr<common::HistogramCuts> cuts,
                        bool is_dense, bst_idx_t row_stride,
                        common::Span<FeatureType const> feature_types,
                        std::shared_ptr<SparsePageSource> source, EllpackCacheInfo const& cinfo)
      : Super{cinfo.missing, ctx->Threads(), n_features, n_batches, cache, false},
        is_dense_{is_dense},
        row_stride_{row_stride},
        param_{std::move(cinfo.param)},
        feature_types_{feature_types} {
    this->source_ = source;
    cuts->SetDevice(ctx->Device());
    this->SetCuts(std::move(cuts), ctx->Device(), cinfo);
    this->Fetch();
  }

  void Fetch() final;
};

// Cache to host
using EllpackPageHostSource =
    EllpackPageSourceImpl<EllpackCacheStreamPolicy<EllpackPage, EllpackFormatPolicy>>;

// Cache to disk
using EllpackPageSource =
    EllpackPageSourceImpl<EllpackMmapStreamPolicy<EllpackPage, EllpackFormatPolicy>>;

/**
 * @brief Ellpack source directly interfaces with user-defined iterators.
 */
template <typename FormatCreatePolicy>
class ExtEllpackPageSourceImpl : public ExtQantileSourceMixin<EllpackPage, FormatCreatePolicy> {
  using Super = ExtQantileSourceMixin<EllpackPage, FormatCreatePolicy>;

  Context const* ctx_;
  BatchParam p_;
  DMatrixProxy* proxy_;
  MetaInfo* info_;
  ExternalDataInfo ext_info_;

 public:
  ExtEllpackPageSourceImpl(
      Context const* ctx, MetaInfo* info, ExternalDataInfo ext_info, std::shared_ptr<Cache> cache,
      std::shared_ptr<common::HistogramCuts> cuts,
      std::shared_ptr<DataIterProxy<DataIterResetCallback, XGDMatrixCallbackNext>> source,
      DMatrixProxy* proxy, EllpackCacheInfo const& cinfo)
      : Super{cinfo.missing, ctx->Threads(), static_cast<bst_feature_t>(info->num_col_), source,
              cache},
        ctx_{ctx},
        p_{cinfo.param},
        proxy_{proxy},
        info_{info},
        ext_info_{std::move(ext_info)} {
    cuts->SetDevice(ctx->Device());
    this->SetCuts(std::move(cuts), ctx->Device(), cinfo);
    CHECK(!this->cache_info_->written);
    this->source_->Reset();
    CHECK(this->source_->Next());
    this->Fetch();
  }

  void Fetch() final;
  // Need a specialized end iter as we can concatenate pages.
  void EndIter() final {
    if (this->cache_info_->written) {
      CHECK_EQ(this->Iter(), this->cache_info_->Size());
    } else {
      CHECK_LE(this->cache_info_->Size(), this->ext_info_.n_batches);
    }
    this->cache_info_->Commit();
    CHECK_GE(this->count_, 1);
    this->count_ = 0;
  }
};

// Cache to host
using ExtEllpackPageHostSource =
    ExtEllpackPageSourceImpl<EllpackCacheStreamPolicy<EllpackPage, EllpackFormatPolicy>>;

// Cache to disk
using ExtEllpackPageSource =
    ExtEllpackPageSourceImpl<EllpackMmapStreamPolicy<EllpackPage, EllpackFormatPolicy>>;

#if !defined(XGBOOST_USE_CUDA)
template <typename F>
inline void EllpackPageSourceImpl<F>::Fetch() {
  // silent the warning about unused variables.
  (void)(row_stride_);
  (void)(is_dense_);
  common::AssertGPUSupport();
}

template <typename F>
inline void ExtEllpackPageSourceImpl<F>::Fetch() {
  common::AssertGPUSupport();
}
#endif  // !defined(XGBOOST_USE_CUDA)
}  // namespace xgboost::data

#endif  // XGBOOST_DATA_ELLPACK_PAGE_SOURCE_H_
