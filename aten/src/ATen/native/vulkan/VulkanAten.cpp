#include <ATen/native/vulkan/VulkanAten.h>
#include <ATen/ATen.h>
#include <ATen/Config.h>
#include <ATen/native/UpSample.h>
#include <ATen/native/utils/ParamUtils.h>
#include <ATen/native/vulkan/Vulkan.h>
#include <ATen/native/vulkan/VulkanOpaqueTensorImpl.h>
#include <ATen/native/vulkan/VulkanOps.h>
#include <ATen/vulkan/Context.h>

namespace at {
namespace native {
namespace vulkan {
namespace aten {
using at::native::vulkan::detail::VulkanTensor;
using VulkanTensorImpl = VulkanOpaqueTensorImpl<VulkanTensor>;

Tensor new_with_vtensor_vulkan(
    VulkanTensor&& vt,
    const TensorOptions& options) {
  auto sizes = vt.sizes();
  auto strides = vt.strides();
  return at::detail::make_tensor<VulkanTensorImpl>(
      DispatchKeySet(DispatchKey::Vulkan),
      options.dtype(),
      at::Device(at::kVulkan),
      std::move(vt),
      std::vector<int64_t>(sizes.begin(), sizes.end()),
      std::vector<int64_t>(strides.begin(), strides.end()));
}

VulkanTensor& vtensor_from_vulkan(const Tensor& tensor) {
  TORCH_INTERNAL_ASSERT(
      tensor.is_vulkan(), "vtensor_from_vulkan expects Vulkan tensor input");
  VulkanTensorImpl* impl =
      static_cast<VulkanTensorImpl*>(tensor.unsafeGetTensorImpl());
  return impl->unsafe_opaque_handle();
}

Tensor empty(
    IntArrayRef size,
    optional<ScalarType> dtype,
    optional<Layout> layout,
    optional<Device> device,
    optional<bool> pin_memory,
    optional<MemoryFormat> memory_format) {
  TORCH_CHECK(
      !pin_memory.has_value(),
      "'pin_memory' argument is incompatible with Vulkan tensor");
  TORCH_CHECK(
      !memory_format.has_value(),
      "'memory_format' argument is incompatible with Vulkan tensor");
  VulkanTensor vt{size.vec()};
  return new_with_vtensor_vulkan(
      std::move(vt), at::device(at::kVulkan).dtype(dtype));
}

Tensor empty_strided(
    IntArrayRef size,
    IntArrayRef stride,
    optional<ScalarType> dtype,
    optional<Layout> layout,
    optional<Device> device,
    optional<bool> pin_memory) {
  return vulkan::aten::empty(
      size, dtype, layout, device, pin_memory, c10::nullopt);
}

Tensor upsample_nearest2d(
    const Tensor& input,
    IntArrayRef outputSizes,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  VulkanTensor& x = vtensor_from_vulkan(input);
  auto inputSizes = input.sizes();
  auto in = inputSizes[0];
  auto ic = inputSizes[1];
  auto ih = inputSizes[2];
  auto iw = inputSizes[3];

  auto oh = outputSizes[0];
  auto ow = outputSizes[1];
  const float height_scale = compute_scales_value<float>(scales_h, ih, oh);
  const float width_scale = compute_scales_value<float>(scales_w, iw, ow);
  VulkanTensor output = VulkanTensor{{in, ic, oh, ow}};
  output.allocate_storage();
  vulkan::detail::upsample_nearest2d(
      output, x, ih, iw, oh, ow, in, ic, height_scale, width_scale);
  return new_with_vtensor_vulkan(std::move(output), input.options());
}

Tensor adaptive_avg_pool2d(const at::Tensor& input, IntArrayRef outputSize) {
  TORCH_INTERNAL_ASSERT(
      input.dim() == 4,
      "vulkan_adaptive_avg_pool2d expects 4-dimensional input");
  auto& x = vtensor_from_vulkan(input);
  auto inputSize = input.sizes();
  auto in = inputSize[0];
  auto ic = inputSize[1];
  auto ih = inputSize[2];
  auto iw = inputSize[3];

  auto oh = outputSize[0];
  auto ow = outputSize[1];
  VulkanTensor output{{in, ic, oh, ow}};
  output.allocate_storage();
  vulkan::detail::adaptive_avg_pool2d(output, x, ih, iw, oh, ow, in, ic);
  return new_with_vtensor_vulkan(std::move(output), input.options());
}

Tensor add(const Tensor& self, const Tensor& other, Scalar alpha) {
  auto xt = self.is_vulkan() ? self : self.vulkan();
  const auto& x = vtensor_from_vulkan(xt);
  auto yt = other.is_vulkan() ? other : other.vulkan();
  const auto& y = vtensor_from_vulkan(yt);
  float a = alpha.to<float>();

  VulkanTensor output{self.sizes().vec()};
  output.allocate_storage();
  vulkan::detail::add(output, x, y, a);
  return new_with_vtensor_vulkan(std::move(output), self.options());
}

Tensor convolution(
    const Tensor& input, // Vulkan
    const Tensor& weight, // CPU
    const Tensor& bias, // CPU
    IntArrayRef stride,
    IntArrayRef padding,
    IntArrayRef dilation,
    bool transposed,
    IntArrayRef output_padding,
    int64_t groups) {
  vulkan::Conv2DParams params{
      input.sizes(), weight.sizes(), padding, stride, dilation, groups};
  TORCH_INTERNAL_ASSERT(
      input.dim() == 4, "convolution: Expected 4-dimensional input");
  TORCH_INTERNAL_ASSERT(
      weight.dim() == 4, "convolution: Expected 4-dimensional weight");
  TORCH_INTERNAL_ASSERT(
      groups == 1 || groups == params.C,
      "convolution: only nogroup or depthwise convolutions supported");
  TORCH_INTERNAL_ASSERT(!transposed, "convolution: transposed not supported");

  const VulkanTensor& vinput = vtensor_from_vulkan(input);
  VulkanTensor voutput = VulkanTensor{params.output_sizes()};
  voutput.allocate_storage();

  vulkan::detail::conv2d(
      voutput,
      vinput,
      weight.data_ptr<float>(),
      bias.defined() ? c10::make_optional<const float*>(bias.data_ptr<float>())
                     : c10::nullopt,
      params);
  return new_with_vtensor_vulkan(std::move(voutput), input.options());
}

Tensor addmm(
    const Tensor& self,
    const Tensor& mat1,
    const Tensor& mat2,
    Scalar beta,
    Scalar alpha) {
  const VulkanTensor t =
      vtensor_from_vulkan(self.is_vulkan() ? self : self.vulkan());
  const VulkanTensor m1 =
      vtensor_from_vulkan(mat1.is_vulkan() ? mat1 : mat1.vulkan());
  const VulkanTensor m2 =
      vtensor_from_vulkan(mat2.is_vulkan() ? mat2 : mat2.vulkan());
  float b = beta.to<float>();
  float a = alpha.to<float>();

  VulkanTensor output = VulkanTensor{self.sizes().vec()};
  output.allocate_storage();
  vulkan::detail::addmm(output, t, m1, m2, b, a);
  return new_with_vtensor_vulkan(std::move(output), self.options());
}

Tensor mm(const Tensor& self, const Tensor& mat2) {
  TORCH_INTERNAL_ASSERT(
      self.dim() == 2 && mat2.dim() == 2,
      "vulkan_mm expects 2-dimensional tensors");
  const auto m1Sizes = self.sizes();
  const auto m2Sizes = mat2.sizes();
  TORCH_INTERNAL_ASSERT(
      m1Sizes[1] == m2Sizes[0],
      "vulkan_mm expects self.sizes[1] equal mat2.sizes[0]");

  const auto& m1 = vtensor_from_vulkan(self.is_vulkan() ? self : self.vulkan());
  const auto& m2 = vtensor_from_vulkan(mat2.is_vulkan() ? mat2 : mat2.vulkan());

  VulkanTensor output{{m1Sizes[0], m2Sizes[1]}};
  output.allocate_storage();
  vulkan::detail::addmm(output, c10::nullopt, m1, m2, 0.f, 1.f);
  return new_with_vtensor_vulkan(std::move(output), self.options());
}

Tensor clamp(
    const Tensor& self,
    c10::optional<Scalar> min,
    c10::optional<Scalar> max) {
  VulkanTensor& x = vtensor_from_vulkan(self);
  VulkanTensor output = VulkanTensor{self.sizes().vec()};
  output.allocate_storage();
  float minValue = min.has_value() ? min.value().to<float>()
                                   : std::numeric_limits<float>::min();
  float maxValue = max.has_value() ? max.value().to<float>()
                                   : std::numeric_limits<float>::max();
  vulkan::detail::clamp(output, x, minValue, maxValue);
  return vulkan::aten::new_with_vtensor_vulkan(
      std::move(output), self.options());
}

Tensor& clamp_(
    Tensor& self,
    c10::optional<Scalar> min,
    c10::optional<Scalar> max) {
  auto y = vulkan::aten::clamp(self, min, max);
  self.copy_(y);
  return self;
}

Tensor hardtanh(const Tensor& self, Scalar min, Scalar max) {
  return vulkan::aten::clamp(self, min, max);
}

Tensor& hardtanh_(Tensor& self, Scalar min, Scalar max) {
  return vulkan::aten::clamp_(self, min, max);
}

Tensor mean(
    const Tensor& self,
    IntArrayRef dim,
    bool keepdim,
    optional<ScalarType> dtype) {
  TORCH_INTERNAL_ASSERT(self.is_vulkan(), "mean expects Vulkan tensor input");
  TORCH_INTERNAL_ASSERT(
      self.dim() == 4 && dim.size() == 2 && dim[0] == 2 && dim[1] == 3);
  VulkanTensor& x = vtensor_from_vulkan(self);
  auto sizes = self.sizes();
  std::vector<int64_t> outputSizes{sizes[0], sizes[1]};
  VulkanTensor output = VulkanTensor{outputSizes};
  output.allocate_storage();
  vulkan::detail::mean(output, x);
  return new_with_vtensor_vulkan(std::move(output), self.options());
}

TORCH_LIBRARY_IMPL(aten, Vulkan, m) {
  m.impl("empty.memory_format", TORCH_FN(at::native::vulkan::aten::empty));
  m.impl("empty_strided", TORCH_FN(at::native::vulkan::aten::empty_strided));
  m.impl("add.Tensor", TORCH_FN(at::native::vulkan::aten::add));
  m.impl("clamp", TORCH_FN(at::native::vulkan::aten::clamp));
  m.impl("mean.dim", TORCH_FN(at::native::vulkan::aten::mean));
  m.impl("mm", TORCH_FN(at::native::vulkan::aten::mm));
  m.impl("addmm", TORCH_FN(at::native::vulkan::aten::addmm));
  m.impl(
      "upsample_nearest2d",
      TORCH_FN(at::native::vulkan::aten::upsample_nearest2d));
  m.impl(
      "_adaptive_avg_pool2d",
      TORCH_FN(at::native::vulkan::aten::adaptive_avg_pool2d));
  m.impl_UNBOXED(
      "convolution_overrideable", at::native::vulkan::aten::convolution);
  m.impl_UNBOXED("hardtanh_", at::native::vulkan::aten::hardtanh_);
}

Tensor& copy_from_vulkan_(Tensor& self, const Tensor& src) {
  TORCH_INTERNAL_ASSERT(
      src.device().type() == DeviceType::Vulkan,
      "copy_from_vulkan input tensor's device is not Vulkan");
  TORCH_INTERNAL_ASSERT(
      self.device().type() == DeviceType::CPU,
      "copy_from_vulkan is implemented only for CPU device output");
  TORCH_INTERNAL_ASSERT(
      self.layout() == Layout::Strided,
      "copy_from_vulkan is implemented only for Strided layout output");
  TORCH_INTERNAL_ASSERT(
      self.scalar_type() == ScalarType::Float,
      "copy_from_vulkan is implemented only for float dtype output, got:",
      self.scalar_type());
  TORCH_INTERNAL_ASSERT(
      self.is_contiguous(),
      "copy_from_vulkan is implemented only for contiguous output tensor");

  VulkanTensor& vtensor = vtensor_from_vulkan(src);
  vtensor.copy_data_to_host(self.data_ptr<float>());
  return self;
}

Tensor& copy_to_vulkan_(Tensor& self, const Tensor& src) {
  TORCH_INTERNAL_ASSERT(
      self.device().type() == DeviceType::Vulkan,
      "copy_to_vulkan output tensor's device is not Vulkan");
  TORCH_INTERNAL_ASSERT(
      src.device().type() == DeviceType::CPU,
      "copy_to_vulkan is implemented only for CPU device input");
  TORCH_INTERNAL_ASSERT(
      src.layout() == Layout::Strided,
      "copy_to_vulkan is implemented only for Strided layout input");
  TORCH_INTERNAL_ASSERT(
      src.scalar_type() == ScalarType::Float,
      "copy_to_vulkan is implemented only for float dtype");

  auto cpu_tensor_contiguous = src.contiguous();
  VulkanTensor& vtensor = vtensor_from_vulkan(self);
  vtensor.set_data_from_host(cpu_tensor_contiguous.data_ptr<float>());
  return self;
}

Tensor& vulkan_copy_impl_(Tensor& self, const Tensor& src) {
  if (src.device().type() == at::kVulkan && self.device().type() == at::kCPU) {
    return copy_from_vulkan_(self, src);
  }
  if (src.device().type() == at::kCPU && self.device().type() == at::kVulkan) {
    return copy_to_vulkan_(self, src);
  }
  TORCH_INTERNAL_ASSERT(
      src.device().type() == DeviceType::Vulkan,
      "vulkan_copy_ is implemented only for CPU,Strided,float->Vulkan; Vulkan->CPU,Strided,float");
  return self;
}

struct VulkanImpl final : public at::vulkan::VulkanImplInterface {
  bool is_vulkan_available() const override {
    return at::native::vulkan::detail::is_available();
  }

  Tensor& vulkan_copy_(Tensor& self, const Tensor& src) const override {
    return vulkan_copy_impl_(self, src);
  }
};
static at::vulkan::VulkanImplRegistrar g_vulkan_impl(new VulkanImpl());

} // namespace aten

using detail::VulkanTensor;
Tensor convolution_prepack_weights(const Tensor& weight) {
  auto wsizes = weight.sizes();
  TORCH_INTERNAL_ASSERT(
      wsizes.size() == 4,
      "convolution_prepack_weights: Expected 4-dimensional weight");

  const int64_t OC = wsizes[0];
  const int64_t C = wsizes[1];
  const int64_t KH = wsizes[2];
  const int64_t KW = wsizes[3];
  VulkanTensor voutput =
      VulkanTensor{{UP_DIV(OC, 4), UP_DIV(C, 4), KH * KW, 16}};
  voutput.allocate_storage();

  vulkan::detail::conv2d_prepack_weights(
      voutput, weight.data_ptr<float>(), OC, C, KH, KW);
  return aten::new_with_vtensor_vulkan(
      std::move(voutput), at::device(at::kVulkan).dtype(at::kFloat));
}

Tensor convolution_prepacked(
    const Tensor& input, // Vulkan
    IntArrayRef weightSizes,
    const Tensor& weight_prepacked_vulkan, // Vulkan
    const c10::optional<Tensor>& bias, // Vulkan|CPU
    IntArrayRef padding,
    IntArrayRef stride,
    IntArrayRef dilation,
    int64_t groups,
    const float output_min,
    const float output_max) {
  TORCH_INTERNAL_ASSERT(
      input.dim() == 4, "Vulkan convolution: Expected 4-dimensional input");
  TORCH_INTERNAL_ASSERT(
      weight_prepacked_vulkan.dim() == 4,
      "Vulkan convolution: Expected 4-dimensional weight");
  vulkan::Conv2DParams params{
      input.sizes(), weightSizes, padding, stride, dilation, groups};
  TORCH_INTERNAL_ASSERT(
      groups == 1 || groups == params.C,
      "Vulkan convolution: only nogroup or depthwise convolutions supported");
  const VulkanTensor& vinput = aten::vtensor_from_vulkan(input);
  const VulkanTensor& vweight =
      aten::vtensor_from_vulkan(weight_prepacked_vulkan);
  VulkanTensor voutput =
      VulkanTensor{{params.N, params.OC, params.OH, params.OW}};
  voutput.allocate_storage();
  const bool hasBias = bias.has_value() && bias->defined();
  const bool vulkanBias = (*bias).is_vulkan();
  if (hasBias && vulkanBias) {
    const VulkanTensor& vbias = aten::vtensor_from_vulkan(*bias);
    vulkan::detail::conv2d(
        voutput, vinput, vweight, vbias, params, output_min, output_max);
  } else {
    vulkan::detail::conv2d(
        voutput,
        vinput,
        vweight,
        hasBias ? c10::make_optional<const float*>((*bias).data_ptr<float>())
                : c10::nullopt,
        params,
        output_min,
        output_max);
  }
  return aten::new_with_vtensor_vulkan(std::move(voutput), input.options());
}

} // namespace vulkan
} // namespace native
} // namespace at
