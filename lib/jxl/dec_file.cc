// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lib/jxl/dec_file.h"

#include <stddef.h>

#include <utility>
#include <vector>

#include "jxl/decode.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/override.h"
#include "lib/jxl/base/profiler.h"
#include "lib/jxl/color_management.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dec_bit_reader.h"
#include "lib/jxl/dec_frame.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/headers.h"
#include "lib/jxl/icc_codec.h"
#include "lib/jxl/image_bundle.h"

namespace jxl {
namespace {

Status DecodeHeaders(BitReader* reader, CodecInOut* io) {
  JXL_RETURN_IF_ERROR(ReadSizeHeader(reader, &io->metadata.size));

  JXL_RETURN_IF_ERROR(ReadImageMetadata(reader, &io->metadata.m));

  io->metadata.transform_data.nonserialized_xyb_encoded =
      io->metadata.m.xyb_encoded;
  JXL_RETURN_IF_ERROR(Bundle::Read(reader, &io->metadata.transform_data));

  return true;
}

}  // namespace

Status DecodePreview(const DecompressParams& dparams,
                     const CodecMetadata& metadata,
                     BitReader* JXL_RESTRICT reader, AuxOut* aux_out,
                     ThreadPool* pool, ImageBundle* JXL_RESTRICT preview,
                     uint64_t* dec_pixels) {
  // No preview present in file.
  if (!metadata.m.have_preview) {
    if (dparams.preview == Override::kOn) {
      return JXL_FAILURE("preview == kOn but no preview present");
    }
    return true;
  }

  // Have preview; prepare to skip or read it.
  JXL_RETURN_IF_ERROR(reader->JumpToByteBoundary());

  if (dparams.preview == Override::kOff) {
    JXL_RETURN_IF_ERROR(SkipFrame(metadata, reader, /*is_preview=*/true));
    return true;
  }

  // Else: default or kOn => decode preview.
  PassesDecoderState dec_state;
  JXL_RETURN_IF_ERROR(DecodeFrame(dparams, &dec_state, pool, reader, aux_out,
                                  preview, metadata, nullptr,
                                  /*is_preview=*/true));
  if (dec_pixels) {
    *dec_pixels += dec_state.shared->frame_dim.xsize_upsampled *
                   dec_state.shared->frame_dim.ysize_upsampled;
  }
  return true;
}

// To avoid the complexity of file I/O and buffering, we assume the bitstream
// is loaded (or for large images/sequences: mapped into) memory.
Status DecodeFile(const DecompressParams& dparams,
                  const Span<const uint8_t> file, CodecInOut* JXL_RESTRICT io,
                  AuxOut* aux_out, ThreadPool* pool) {
  PROFILER_ZONE("DecodeFile uninstrumented");

  // Marker
  JxlSignature signature = JxlSignatureCheck(file.data(), file.size());
  if (signature == JXL_SIG_NOT_ENOUGH_BYTES || signature == JXL_SIG_INVALID) {
    return JXL_FAILURE("File does not start with known JPEG XL signature");
  }

  std::unique_ptr<jpeg::JPEGData> jpeg_data = nullptr;
  if (dparams.keep_dct) {
    if (io->Main().jpeg_data == nullptr) {
      return JXL_FAILURE("Caller must set jpeg_data");
    }
    jpeg_data = std::move(io->Main().jpeg_data);
  }

  Status ret = true;
  {
    BitReader reader(file);
    BitReaderScopedCloser reader_closer(&reader, &ret);
    (void)reader.ReadFixedBits<16>();  // skip marker

    {
      JXL_RETURN_IF_ERROR(DecodeHeaders(&reader, io));
      size_t xsize = io->metadata.xsize();
      size_t ysize = io->metadata.ysize();
      JXL_RETURN_IF_ERROR(io->VerifyDimensions(xsize, ysize));
    }

    if (io->metadata.m.color_encoding.WantICC()) {
      PaddedBytes icc;
      JXL_RETURN_IF_ERROR(ReadICC(&reader, &icc));
      JXL_RETURN_IF_ERROR(io->metadata.m.color_encoding.SetICC(std::move(icc)));
    }
    // Set ICC profile in jpeg_data.
    if (jpeg_data) {
      const auto& icc = io->metadata.m.color_encoding.ICC();
      size_t icc_pos = 0;
      for (size_t i = 0; i < jpeg_data->app_data.size(); i++) {
        if (jpeg_data->app_marker_type[i] != jpeg::AppMarkerType::kICC) {
          continue;
        }
        size_t len = jpeg_data->app_data[i].size() - 17;
        if (icc_pos + len > icc.size()) {
          return JXL_FAILURE(
              "ICC length is less than APP markers: requested %zu more bytes, "
              "%zu available",
              len, icc.size() - icc_pos);
        }
        memcpy(&jpeg_data->app_data[i][17], icc.data() + icc_pos, len);
        icc_pos += len;
      }
      if (icc_pos != icc.size() && icc_pos != 0) {
        return JXL_FAILURE("ICC length is more than APP markers");
      }
    }

    JXL_RETURN_IF_ERROR(DecodePreview(dparams, io->metadata, &reader, aux_out,
                                      pool, &io->preview_frame,
                                      &io->dec_pixels));

    // Only necessary if no ICC and no preview.
    JXL_RETURN_IF_ERROR(reader.JumpToByteBoundary());
    if (io->metadata.m.have_animation && dparams.keep_dct) {
      return JXL_FAILURE("Cannot decode to JPEG an animation");
    }

    PassesDecoderState dec_state;
    // OK to depend on a CMS here, as DecodeFile is never called from the C API.
    dec_state.do_colorspace_transform =
        [](ImageBundle* ib, const ColorEncoding& c_desired, ThreadPool* pool) {
          return ib->TransformTo(c_desired, pool);
        };

    io->frames.clear();
    do {
      io->frames.emplace_back(&io->metadata.m);
      if (jpeg_data) {
        io->frames.back().jpeg_data = std::move(jpeg_data);
      }
      // Skip frames that are not displayed.
      do {
        JXL_RETURN_IF_ERROR(DecodeFrame(dparams, &dec_state, pool, &reader,
                                        aux_out, &io->frames.back(),
                                        io->metadata, io));
      } while (dec_state.shared->frame_header.frame_type !=
                   FrameType::kRegularFrame &&
               dec_state.shared->frame_header.frame_type !=
                   FrameType::kSkipProgressive);
      io->dec_pixels += io->frames.back().xsize() * io->frames.back().ysize();
    } while (!dec_state.shared->frame_header.is_last);

    if (dparams.check_decompressed_size && !dparams.allow_partial_files &&
        dparams.max_downsampling == 1) {
      if (reader.TotalBitsConsumed() != file.size() * kBitsPerByte) {
        return JXL_FAILURE("DecodeFile reader position not at EOF.");
      }
    }
    // Suppress errors when decoding partial files with DC frames.
    if (!reader.AllReadsWithinBounds() && dparams.allow_partial_files) {
      (void)reader.Close();
    }

    io->CheckMetadata();
    // reader is closed here.
  }
  return ret;
}

}  // namespace jxl
