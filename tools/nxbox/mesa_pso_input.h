/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Read the emitted container, not the NIR that copy_input_attribs mutates after
 * compilation. ISG1 records contain stream/name/index/system/type/register,
 * then declaration and usage masks. Registers are NOT semantic indices.
 */
struct NxboxInputSignature {
   const unsigned char *data = nullptr;
   size_t size = 0;
   uint32_t count = 0;
   uint32_t offset = 0;

   static uint32_t word(const unsigned char *p) {
      return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
   }
   const unsigned char *element(uint32_t i) const { return data + offset + size_t(i) * 32; }
   const char *name(const unsigned char *e) const {
      return reinterpret_cast<const char *>(data + word(e + 4));
   }
   bool parse(const D3D12_SHADER_BYTECODE &shader) {
      const auto *bytes = static_cast<const unsigned char *>(shader.pShaderBytecode);
      const size_t length = shader.BytecodeLength;
      if (!bytes || length < 32 || memcmp(bytes, "DXBC", 4) || word(bytes + 24) != length)
         return false;
      const uint32_t parts = word(bytes + 28);
      if (parts > (length - 32) / 4)
         return false;
      bool found = false;
      for (uint32_t i = 0; i < parts; ++i) {
         const size_t part = word(bytes + 32 + size_t(i) * 4);
         if (part < 32 + size_t(parts) * 4 || part > length - 8)
            return false;
         const size_t payload = word(bytes + part + 4);
         if (payload > length - part - 8)
            return false;
         if (memcmp(bytes + part, "ISG1", 4))
            continue;
         if (found || payload < 8)
            return false;
         found = true;
         data = bytes + part + 8;
         size = payload;
         count = word(data);
         offset = word(data + 4);
         if (offset < 8 || offset > size || count > (size - offset) / 32)
            return false;
         for (uint32_t j = 0; j < count; ++j) {
            const auto *e = element(j);
            const size_t string_offset = word(e + 4);
            if (string_offset < offset + size_t(count) * 32 || string_offset >= size ||
                !memchr(data + string_offset, 0, size - string_offset) || (e[24] & ~15u) ||
                (e[25] & ~e[24]))
               return false;
         }
      }
      return found;
   }
   static bool same_name(const char *a, const char *b) {
      if (!a || !b)
         return false;
      do {
         unsigned x = static_cast<unsigned char>(*a++);
         unsigned y = static_cast<unsigned char>(*b++);
         if (x >= 'a' && x <= 'z')
            x -= 'a' - 'A';
         if (y >= 'a' && y <= 'z')
            y -= 'a' - 'A';
         if (x != y)
            return false;
         if (!x)
            return true;
      } while (true);
   }
   bool declares(const D3D12_INPUT_ELEMENT_DESC &input) const {
      for (uint32_t i = 0; i < count; ++i) {
         const auto *e = element(i);
         /* System values (VertexID/InstanceID) come from the runtime, not IA.
          * Keep declared inputs even with read-mask zero: Mesa only populates
          * usage masks for validator >= 1.5. ISG1 alone cannot prove that a
          * zero mask means unused with an older validator. */
         if (!word(e) && !word(e + 12) && word(e + 8) == input.SemanticIndex &&
             same_name(name(e), input.SemanticName))
            return true;
      }
      return false;
   }
};

static bool nxbox_pso_fix_enabled() {
   static const bool enabled = [] {
      char value[4] = {};
      return !(GetEnvironmentVariableA("NXBOX_PSO_FIX", value, sizeof(value)) == 1 &&
               value[0] == '0');
   }();
   return enabled;
}

/* Caller-owned storage must outlive both PSO creation APIs and all retries.
 * Never renumber semantics, narrow formats, or change offsets/divisors. In
 * particular, packed signature rows may have several distinct semantics.
 * APPEND offsets depend on removed predecessors, so leave that layout alone.
 * Unknown/malformed signatures leave the original layout unchanged.
 */
static void nxbox_pso_fix_inputs(const D3D12_SHADER_BYTECODE &vs, D3D12_INPUT_LAYOUT_DESC &layout,
                                 D3D12_INPUT_ELEMENT_DESC *storage, size_t capacity) {
   if (!nxbox_pso_fix_enabled() || layout.NumElements > capacity ||
       (layout.NumElements && !layout.pInputElementDescs))
      return;
   NxboxInputSignature signature;
   if (!signature.parse(vs))
      return;
   for (UINT i = 0; i < layout.NumElements; ++i)
      if (layout.pInputElementDescs[i].AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT)
         return;
   /* Do not hide an existing missing-semantic problem by removing other
    * entries. Remapping or padding would require knowing the original NIR
    * attribute-to-buffer mapping, not just the compiled signature. */
   for (uint32_t i = 0; i < signature.count; ++i) {
      const auto *e = signature.element(i);
      if (signature.word(e) || signature.word(e + 12))
         continue;
      bool matched = false;
      for (UINT j = 0; j < layout.NumElements; ++j) {
         const auto &input = layout.pInputElementDescs[j];
         matched |= signature.word(e + 8) == input.SemanticIndex &&
                    signature.same_name(signature.name(e), input.SemanticName);
      }
      if (!matched)
         return;
   }
   UINT kept = 0;
   for (UINT i = 0; i < layout.NumElements; ++i)
      if (signature.declares(layout.pInputElementDescs[i]))
         storage[kept++] = layout.pInputElementDescs[i];
   if (kept != layout.NumElements) {
      char text[128];
      snprintf(text, sizeof(text), "inputs=%u kept=%u signature=%u", layout.NumElements, kept,
               signature.count);
      SetEnvironmentVariableA("NXBOX_D3D12_PSO_FIX", text);
      layout = {kept ? storage : nullptr, kept};
   }
}

/* The console report also contains zero (invalid enum) blend factors/ops on a
 * disabled target. Canonicalize only ignored fields before the FIRST create:
 * retrying after an Xbox device-removing rejection is already too late.
 */
static void nxbox_pso_fix_blend(D3D12_BLEND_DESC &blend) {
   if (!nxbox_pso_fix_enabled())
      return;
   for (auto &target : blend.RenderTarget) {
      if (!target.BlendEnable) {
         target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
         target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
         target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      }
      if (!target.LogicOpEnable)
         target.LogicOp = D3D12_LOGIC_OP_NOOP;
   }
}

/* Exact emitted shader pairs from BotW pipe6-6a7affd1e and MK8D pipe6-3dff688b0. Sizes or IA
 * semantics alone would suppress unrelated draws. This deliberately drops the
 * affected draw; it does not claim to repair the driver or shader. Keep active
 * even with NXBOX_PSO_FIX=0, which only controls IA/blend normalization.
 */
static uint64_t nxbox_shader_hash(const D3D12_SHADER_BYTECODE &shader) {
   uint64_t hash = 14695981039346656037ull;
   const auto *bytes =
       static_cast<const unsigned char *>(shader.pShaderBytecode);
   for (size_t i = 0; i < shader.BytecodeLength; ++i)
      hash = (hash ^ bytes[i]) * 1099511628211ull;
   return hash;
}

static bool nxbox_pso_quarantine_enabled() {
   static const bool enabled = [] {
      char value[4] = {};
      return !(GetEnvironmentVariableA("NXBOX_PSO_QUARANTINE", value, sizeof(value)) == 1 &&
               value[0] == '0');
   }();
   return enabled;
}

static bool nxbox_pso_quarantined(const D3D12_SHADER_BYTECODE &vs,
                                  const D3D12_SHADER_BYTECODE &ps) {
   /* NXBOX_PSO_QUARANTINE=0 lets the known device-removing pairs through, to test whether the
    * skipped draw is what leaves the picture black. */
   if (!nxbox_pso_quarantine_enabled() || !vs.pShaderBytecode || !ps.pShaderBytecode)
      return false;
   return (vs.BytecodeLength == 2492 && ps.BytecodeLength == 2192 &&
           nxbox_shader_hash(vs) == 0xfee40f01e55da8b0ull &&
           nxbox_shader_hash(ps) == 0x944713566752175eull) ||
          (vs.BytecodeLength == 2304 && ps.BytecodeLength == 2212 &&
           nxbox_shader_hash(vs) == 0xa9d00cddd91828bcull &&
           nxbox_shader_hash(ps) == 0x789a1e9e39699088ull);
}
