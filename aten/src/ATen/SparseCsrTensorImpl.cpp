#include <ATen/ATen.h>
#include <ATen/InitialTensorOptions.h>
#include <ATen/SparseCsrTensorImpl.h>
#include <ATen/SparseCsrTensorUtils.h>
#include <ATen/SparseTensorImpl.h>
#include <ATen/SparseTensorUtils.h>
#include <ATen/core/LegacyTypeDispatch.h>
#include <ATen/native/Resize.h>

namespace at {
namespace {
DeviceType SparseCsrTensorSetToDeviceType(DispatchKeySet key_set) {
  if (key_set.has(DispatchKey::SparseCsrCPU)) {
    return kCPU;
  } else if (key_set.has(DispatchKey::SparseCsrCUDA)) {
    return kCUDA;
  } else {
    TORCH_CHECK(false,
        "Cannot construct SparseCsrTensor with non-sparse tensor type ID ",
        key_set);
  }
}
} // namespace

SparseCsrTensorImpl::SparseCsrTensorImpl(
    at::DispatchKeySet key_set,
    at::Layout layout,
    const caffe2::TypeMeta data_type)
    : SparseCsrTensorImpl(
          key_set,
          data_type,
          at::empty(
              {0},
              at::initialTensorOptions()
                  .device(SparseCsrTensorSetToDeviceType(key_set))
                  .dtype(ScalarType::Int)) // crow_indices
          ,
          at::empty(
              {0},
              at::initialTensorOptions()
                  .device(SparseCsrTensorSetToDeviceType(key_set))
                  .dtype(ScalarType::Int)) // col_indices
          ,
          at::empty(
              {0},
              at::initialTensorOptions()
                  .device(SparseCsrTensorSetToDeviceType(key_set))
                  .dtype(data_type)) // values
          ,
          layout
      ) {}

SparseCsrTensorImpl::SparseCsrTensorImpl(
    at::DispatchKeySet key_set,
    const caffe2::TypeMeta data_type,
    at::Tensor crow_indices,
    at::Tensor col_indices,
    at::Tensor values,
    at::Layout layout)
    : TensorImpl(key_set, data_type, values.device()),
      crow_indices_(std::move(crow_indices)),
      col_indices_(std::move(col_indices)),
      values_(std::move(values)),
      layout_(layout) {
  // https://pytorch.org/blog/pytorch-feature-classification-changes/#beta
  TORCH_WARN_ONCE("Sparse ", at::sparse_csr::layoutToString(layout_, /*upper=*/true), " tensor support is in beta state. "
                  "If you miss a functionality in the sparse tensor support, please submit a feature request "
                  "to https://github.com/pytorch/pytorch/issues.");
  set_storage_access_should_throw();
  is_non_overlapping_and_dense_ = false;
  set_custom_sizes_strides(SizesStridesPolicy::CustomStrides);
  // TODO: If this check ever shows up as a bottleneck, which is unlikely given that
  // comparing devices only involves comparing the type and index (two integers), we
  // can move this to a DEBUG only assert. Until then this confirms and maintains a
  // crucial invariance.
  TORCH_CHECK(values_.device() == crow_indices_.device(), "Values and crow_indices need to be on the same device.");
  TORCH_CHECK(values_.device() == col_indices_.device(), "Values and col_indices need to be on the same device.");
}

const char* SparseCsrTensorImpl::tensorimpl_type_name() const {
  return "SparseCsrTensorImpl";
}

void SparseCsrTensorImpl::resize_(int64_t nnz, IntArrayRef size) {
  TORCH_CHECK(
      !has_symbolic_sizes_strides_,
      "resize_ called on tensor with symbolic shape")
  auto rows = size[size.size() - 2];
  auto cols = size[size.size() - 1];
  auto old_crow_indices_size = crow_indices_.size(-1);

  auto new_crow_indices_size = DimVector(size.slice(0, size.size() - 2));
  new_crow_indices_size.push_back(rows + 1);
  crow_indices_.resize_(new_crow_indices_size);
  if (rows + 1 >= old_crow_indices_size) {
    crow_indices_.narrow(-1, old_crow_indices_size, rows + 1 - old_crow_indices_size).fill_(nnz);
  } else {
    crow_indices_.narrow(-1, rows, 1).fill_(std::min<int64_t>(nnz, rows*cols));
  }
  auto col_indices_values_size = DimVector(size.slice(0, size.size() - 2));
  col_indices_values_size.push_back(std::min<int64_t>(nnz, rows*cols));
  col_indices_.resize_(col_indices_values_size);
  values_.resize_(col_indices_values_size);
  sizes_and_strides_.set_sizes(size);
}

void SparseCsrTensorImpl::resize_and_clear_(int64_t sparse_dim, IntArrayRef size) {
  TORCH_CHECK(
      !has_symbolic_sizes_strides_,
      "resize_as_sparse_csr_tensor_ called on tensor with symbolic shape");
  TORCH_CHECK(sparse_dim >= 2, "resize_and_clear_ sparse dimensionality must be at least 2, got ", sparse_dim);
  TORCH_CHECK(static_cast<int64_t>(size.size()) >= sparse_dim, "resize_and_clear_ size length must be at least sparse dimensionality (=",
              sparse_dim, "), got ", size.size());
  auto batch_dim = sparse_dim - 2;
  auto batchsize = size.slice(0, batch_dim);
  auto densesize = size.slice(batch_dim + 2, size.size() - batch_dim - 2);

  auto values_size = DimVector(batchsize);
  values_size.push_back(0); // nse
  values_size.append(densesize.begin(), densesize.end());

  auto col_indices_size = DimVector(batchsize);
  col_indices_size.push_back(0); // nse

  auto n_compressed_indices = AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(layout_, "resize_and_clear_",
                                                                        [&] () -> int64_t { return size[batch_dim]; },
                                                                        [&] () -> int64_t { return size[batch_dim + 1]; }
                                                                        );
  AT_DISPATCH_PLAIN_SPARSE_COMPRESSED_LAYOUTS(layout_,
                                              "resize_and_clear_",
                                              [] () {},
                                              [&] () {
                                                auto blocksize = this->values_.sizes().slice(this->batch_dim() + 1, 2);
                                                values_size.append(blocksize.begin(), blocksize.end());
                                                n_compressed_indices /= blocksize[(the_layout == kSparseBsr ? 0 : 1)];
                                              });
  auto crow_indices_size = DimVector(batchsize);
  crow_indices_size.push_back(n_compressed_indices + 1);

  crow_indices_.resize_(crow_indices_size);
  crow_indices_.zero_();
  col_indices_.resize_(col_indices_size);
  values_.resize_(values_size);
  sizes_and_strides_.set_sizes(size);
  refresh_numel();
}

void SparseCsrTensorImpl::resize_as_sparse_csr_tensor_(const Tensor& src) {
  TORCH_CHECK(
      !has_symbolic_sizes_strides_,
      "resize_as_sparse_csr_tensor_ called on tensor with symbolic shape");
  set_layout(src.layout());
  crow_indices_ = at::empty_like(
      src.crow_indices(),
      src.crow_indices().options(),
      src.crow_indices().suggest_memory_format());
  col_indices_ = at::empty_like(
      src.col_indices(),
      src.col_indices().options(),
      src.col_indices().suggest_memory_format());
  values_ = at::empty_like(
      src.values(),
      src.values().options(),
      src.values().suggest_memory_format());
  sizes_and_strides_.set_sizes(src.sizes());
  refresh_numel();
}

void SparseCsrTensorImpl::set_member_tensors(
    const Tensor& crow_indices,
    const Tensor& col_indices,
    const Tensor& values,
    IntArrayRef size) {
  TORCH_CHECK(
      !has_symbolic_sizes_strides_,
      "set_member_tensors called on tensor with symbolic shape");

  // CSR Type Invariants
  TORCH_CHECK(
      values.scalar_type() == typeMetaToScalarType(dtype()),
      "dtype of values (",
      values.scalar_type(),
      ") must match dtype of sparse tensor (",
      typeMetaToScalarType(dtype()),
      ")");

  crow_indices_ = crow_indices;
  col_indices_ = col_indices;
  values_ = values;

  sizes_and_strides_.set_sizes(size);
  refresh_numel();
  // TODO: If this check ever shows up as a bottleneck, which is unlikely given that
  // comparing devices only involves comparing the type and index (two integers), we
  // can move this to a DEBUG only assert. Until then this confirms and maintains a
  // crucial invariance.
  TORCH_CHECK(values_.device() == crow_indices_.device(), "Values and crow_indices need to be on the same device.");
  TORCH_CHECK(values_.device() == col_indices_.device(), "Values and col_indices need to be on the same device.");
}

IntArrayRef SparseCsrTensorImpl::strides_custom() const {
  TORCH_CHECK(false, "Sparse ", at::sparse_csr::layoutToString(layout_, /*upper=*/true), " tensors do not have strides");
}
SymIntArrayRef SparseCsrTensorImpl::sym_strides_custom() const {
  TORCH_CHECK(false, "Sparse ", at::sparse_csr::layoutToString(layout_, /*upper=*/true), " tensors do not have strides");
}
void SparseCsrTensorImpl::set_size(int64_t dim, int64_t new_size) {
  TORCH_CHECK(false, "Sparse ", at::sparse_csr::layoutToString(layout_, /*upper=*/true), " tensors do not have set_size.");
}
void SparseCsrTensorImpl::set_stride(int64_t dim, int64_t new_stride) {
  TORCH_CHECK(false, "Sparse ", at::sparse_csr::layoutToString(layout_, /*upper=*/true), " tensors do not have set_stride.");
}
void SparseCsrTensorImpl::set_storage_offset(int64_t storage_offset) {
  TORCH_CHECK(false, "Sparse ", at::sparse_csr::layoutToString(layout_, /*upper=*/true), " tensors do not have set_storage_offset.");
}
bool SparseCsrTensorImpl::is_contiguous_custom(MemoryFormat) const {
  TORCH_CHECK(false, "Sparse ", at::sparse_csr::layoutToString(layout_, /*upper=*/true), " tensors do not have is_contiguous");
}

} // namespace at
