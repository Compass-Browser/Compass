// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "compass/media/avf/service/mse/mp4_writer.h"

#include <algorithm>
#include <cstring>

#include "base/check_op.h"

namespace compass::avf::mse {
namespace {

// The unity matrix of mvhd and tkhd.
void WriteMatrix(BoxWriter& w) {
  const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0,
                              0,          0, 0x40000000};
  for (uint32_t value : matrix) {
    w.U32(value);
  }
}

void WriteSinf(BoxWriter& w, uint32_t original_format,
               const Mp4Encryption& encryption) {
  size_t sinf = w.StartBox(FourCC("sinf"));
  size_t frma = w.StartBox(FourCC("frma"));
  w.U32(original_format);
  w.EndBox(frma);
  size_t schm = w.StartFullBox(FourCC("schm"), 0, 0);
  w.U32(FourCC("cbcs"));
  w.U32(0x00010000);
  w.EndBox(schm);
  size_t schi = w.StartBox(FourCC("schi"));
  // Version 1: with the pattern.
  size_t tenc = w.StartFullBox(FourCC("tenc"), 1, 0);
  w.U8(0);
  w.U8(static_cast<uint8_t>((encryption.crypt_byte_block << 4) |
                            (encryption.skip_byte_block & 0x0F)));
  w.U8(1);  // default_isProtected
  bool constant = !encryption.constant_iv.empty();
  w.U8(constant ? 0 : 16);  // default_Per_Sample_IV_Size
  w.Bytes(encryption.key_id.data(), encryption.key_id.size());
  if (constant) {
    w.U8(static_cast<uint8_t>(encryption.constant_iv.size()));
    w.Bytes(encryption.constant_iv);
  }
  w.EndBox(tenc);
  w.EndBox(schi);
  w.EndBox(sinf);
}

void WriteSampleEntry(BoxWriter& w, const Mp4Track& track) {
  bool video = track.kind == Mp4Track::Kind::kVideo;
  uint32_t type = track.format;
  if (track.encryption) {
    type = video ? FourCC("encv") : FourCC("enca");
  }
  size_t entry = w.StartBox(type);
  w.Zeros(6);  // reserved
  w.U16(1);    // data_reference_index
  if (video) {
    w.U16(0);    // pre_defined
    w.U16(0);    // reserved
    w.Zeros(12);  // pre_defined
    w.U16(track.width);
    w.U16(track.height);
    w.U32(0x00480000);  // 72 dpi
    w.U32(0x00480000);
    w.U32(0);   // reserved
    w.U16(1);   // frame_count
    w.Zeros(32);  // compressorname
    w.U16(0x0018);  // depth
    w.U16(0xFFFF);  // pre_defined = -1
  } else {
    w.Zeros(8);  // reserved
    w.U16(track.channels);
    w.U16(16);  // samplesize
    w.U16(0);   // pre_defined
    w.U16(0);   // reserved
    w.U32(track.sample_rate << 16);
  }
  w.Bytes(track.config_box);
  if (track.encryption) {
    WriteSinf(w, track.format, *track.encryption);
  }
  w.EndBox(entry);
}

void WriteTrak(BoxWriter& w, const Mp4Track& track) {
  bool video = track.kind == Mp4Track::Kind::kVideo;
  size_t trak = w.StartBox(FourCC("trak"));

  size_t tkhd = w.StartFullBox(FourCC("tkhd"), 0, 0x3);  // enabled, in movie
  w.U32(0);  // creation_time
  w.U32(0);  // modification_time
  w.U32(track.track_id);
  w.U32(0);  // reserved
  w.U32(0);  // duration
  w.Zeros(8);
  w.U16(0);  // layer
  w.U16(0);  // alternate_group
  w.U16(video ? 0 : 0x0100);  // volume
  w.U16(0);
  WriteMatrix(w);
  w.U32(video ? static_cast<uint32_t>(track.width) << 16 : 0);
  w.U32(video ? static_cast<uint32_t>(track.height) << 16 : 0);
  w.EndBox(tkhd);

  size_t mdia = w.StartBox(FourCC("mdia"));
  size_t mdhd = w.StartFullBox(FourCC("mdhd"), 0, 0);
  w.U32(0);
  w.U32(0);
  w.U32(track.timescale);
  w.U32(0);       // duration
  w.U16(0x55C4);  // 'und'
  w.U16(0);
  w.EndBox(mdhd);

  size_t hdlr = w.StartFullBox(FourCC("hdlr"), 0, 0);
  w.U32(0);
  w.U32(video ? FourCC("vide") : FourCC("soun"));
  w.Zeros(12);
  const char* name = video ? "VideoHandler" : "SoundHandler";
  w.Bytes(reinterpret_cast<const uint8_t*>(name), strlen(name) + 1);
  w.EndBox(hdlr);

  size_t minf = w.StartBox(FourCC("minf"));
  if (video) {
    size_t vmhd = w.StartFullBox(FourCC("vmhd"), 0, 1);
    w.Zeros(8);
    w.EndBox(vmhd);
  } else {
    size_t smhd = w.StartFullBox(FourCC("smhd"), 0, 0);
    w.Zeros(4);
    w.EndBox(smhd);
  }
  size_t dinf = w.StartBox(FourCC("dinf"));
  size_t dref = w.StartFullBox(FourCC("dref"), 0, 0);
  w.U32(1);
  size_t url = w.StartFullBox(FourCC("url "), 0, 1);  // self-contained
  w.EndBox(url);
  w.EndBox(dref);
  w.EndBox(dinf);

  size_t stbl = w.StartBox(FourCC("stbl"));
  size_t stsd = w.StartFullBox(FourCC("stsd"), 0, 0);
  w.U32(1);
  WriteSampleEntry(w, track);
  w.EndBox(stsd);
  for (const char* empty : {"stts", "stsc", "stco"}) {
    size_t box = w.StartFullBox(FourCC(empty), 0, 0);
    w.U32(0);
    w.EndBox(box);
  }
  size_t stsz = w.StartFullBox(FourCC("stsz"), 0, 0);
  w.U32(0);
  w.U32(0);
  w.EndBox(stsz);
  w.EndBox(stbl);
  w.EndBox(minf);
  w.EndBox(mdia);
  w.EndBox(trak);
}

// Per-sample auxiliary information size (saiz) for `sample`.
uint8_t AuxInfoSize(const Mp4Fragment& fragment, const Mp4Sample& sample) {
  size_t size = fragment.per_sample_iv_size;
  if (!sample.subsamples.empty()) {
    size += 2 + 6 * sample.subsamples.size();
  }
  CHECK_LE(size, 255u);
  return static_cast<uint8_t>(size);
}

bool HasSubsamples(const Mp4Fragment& fragment) {
  for (const auto& sample : fragment.samples) {
    if (!sample.subsamples.empty()) {
      return true;
    }
  }
  return false;
}

}  // namespace

BoxWriter::BoxWriter() = default;
BoxWriter::~BoxWriter() = default;

size_t BoxWriter::StartBox(uint32_t type) {
  size_t offset = buffer_.size();
  U32(0);
  U32(type);
  return offset;
}

size_t BoxWriter::StartFullBox(uint32_t type, uint8_t version, uint32_t flags) {
  size_t offset = StartBox(type);
  U8(version);
  U24(flags);
  return offset;
}

void BoxWriter::EndBox(size_t offset) {
  PatchU32(offset, static_cast<uint32_t>(buffer_.size() - offset));
}

void BoxWriter::U8(uint8_t value) {
  buffer_.push_back(value);
}

void BoxWriter::U16(uint16_t value) {
  U8(value >> 8);
  U8(value & 0xFF);
}

void BoxWriter::U24(uint32_t value) {
  U8((value >> 16) & 0xFF);
  U16(value & 0xFFFF);
}

void BoxWriter::U32(uint32_t value) {
  U16(value >> 16);
  U16(value & 0xFFFF);
}

void BoxWriter::U64(uint64_t value) {
  U32(static_cast<uint32_t>(value >> 32));
  U32(static_cast<uint32_t>(value));
}

void BoxWriter::Bytes(const std::vector<uint8_t>& bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void BoxWriter::Bytes(const uint8_t* bytes, size_t size) {
  buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void BoxWriter::Zeros(size_t count) {
  buffer_.insert(buffer_.end(), count, 0);
}

void BoxWriter::PatchU32(size_t offset, uint32_t value) {
  CHECK_LE(offset + 4, buffer_.size());
  buffer_[offset] = value >> 24;
  buffer_[offset + 1] = (value >> 16) & 0xFF;
  buffer_[offset + 2] = (value >> 8) & 0xFF;
  buffer_[offset + 3] = value & 0xFF;
}

std::vector<uint8_t> WriteInitSegment(const std::vector<Mp4Track>& tracks) {
  BoxWriter w;
  size_t ftyp = w.StartBox(FourCC("ftyp"));
  w.U32(FourCC("iso6"));
  w.U32(0);
  w.U32(FourCC("iso6"));
  w.U32(FourCC("cmfc"));
  w.U32(FourCC("mp41"));
  w.EndBox(ftyp);

  size_t moov = w.StartBox(FourCC("moov"));
  size_t mvhd = w.StartFullBox(FourCC("mvhd"), 0, 0);
  w.U32(0);
  w.U32(0);
  w.U32(1000);  // timescale
  w.U32(0);     // duration
  w.U32(0x00010000);  // rate
  w.U16(0x0100);      // volume
  w.Zeros(10);
  WriteMatrix(w);
  w.Zeros(24);  // pre_defined
  uint32_t next_track_id = 1;
  for (const auto& track : tracks) {
    next_track_id = std::max(next_track_id, track.track_id + 1);
  }
  w.U32(next_track_id);
  w.EndBox(mvhd);

  for (const auto& track : tracks) {
    WriteTrak(w, track);
  }

  size_t mvex = w.StartBox(FourCC("mvex"));
  for (const auto& track : tracks) {
    size_t trex = w.StartFullBox(FourCC("trex"), 0, 0);
    w.U32(track.track_id);
    w.U32(1);  // default_sample_description_index
    w.U32(0);
    w.U32(0);
    w.U32(0);
    w.EndBox(trex);
  }
  w.EndBox(mvex);
  w.EndBox(moov);
  return w.Take();
}

std::vector<uint8_t> WriteMediaSegment(
    uint32_t sequence_number,
    const std::vector<Mp4Fragment>& fragments) {
  BoxWriter w;
  size_t moof = w.StartBox(FourCC("moof"));
  size_t mfhd = w.StartFullBox(FourCC("mfhd"), 0, 0);
  w.U32(sequence_number);
  w.EndBox(mfhd);

  // Offsets to patch once the moof's size is known.
  std::vector<size_t> data_offset_fields;
  std::vector<size_t> saio_fields;
  std::vector<size_t> senc_data_offsets;

  for (const auto& fragment : fragments) {
    size_t traf = w.StartBox(FourCC("traf"));
    // default-base-is-moof
    size_t tfhd = w.StartFullBox(FourCC("tfhd"), 0, 0x020000);
    w.U32(fragment.track_id);
    w.EndBox(tfhd);
    size_t tfdt = w.StartFullBox(FourCC("tfdt"), 1, 0);
    w.U64(fragment.base_decode_time);
    w.EndBox(tfdt);

    // data offset, duration, size, flags, composition time offset; version 1
    // for signed composition offsets.
    size_t trun = w.StartFullBox(FourCC("trun"), 1, 0x000F01);
    w.U32(static_cast<uint32_t>(fragment.samples.size()));
    data_offset_fields.push_back(w.size());
    w.U32(0);
    for (const auto& sample : fragment.samples) {
      w.U32(sample.duration);
      w.U32(static_cast<uint32_t>(sample.data.size()));
      // Sync: depends on nothing. Else: depends on others, not a sync sample.
      w.U32(sample.sync ? 0x02000000 : 0x01010000);
      w.U32(static_cast<uint32_t>(sample.composition_offset));
    }
    w.EndBox(trun);

    if (fragment.encrypted) {
      bool subsamples = HasSubsamples(fragment);
      size_t saiz = w.StartFullBox(FourCC("saiz"), 0, 0);
      w.U8(0);  // default_sample_info_size: per sample
      w.U32(static_cast<uint32_t>(fragment.samples.size()));
      for (const auto& sample : fragment.samples) {
        w.U8(AuxInfoSize(fragment, sample));
      }
      w.EndBox(saiz);

      size_t saio = w.StartFullBox(FourCC("saio"), 0, 0);
      w.U32(1);
      saio_fields.push_back(w.size());
      w.U32(0);
      w.EndBox(saio);

      size_t senc = w.StartFullBox(FourCC("senc"), 0, subsamples ? 0x2 : 0);
      w.U32(static_cast<uint32_t>(fragment.samples.size()));
      senc_data_offsets.push_back(w.size());
      for (const auto& sample : fragment.samples) {
        if (fragment.per_sample_iv_size) {
          CHECK_EQ(sample.iv.size(), fragment.per_sample_iv_size);
          w.Bytes(sample.iv);
        }
        if (subsamples) {
          w.U16(static_cast<uint16_t>(sample.subsamples.size()));
          for (const auto& subsample : sample.subsamples) {
            w.U16(static_cast<uint16_t>(subsample.clear_bytes));
            w.U32(subsample.cypher_bytes);
          }
        }
      }
      w.EndBox(senc);
    }
    w.EndBox(traf);
  }
  w.EndBox(moof);
  size_t moof_size = w.size() - moof;

  // The samples: each fragment's in turn, after the mdat header.
  size_t data_offset = moof_size + 8;
  for (size_t i = 0; i < fragments.size(); ++i) {
    w.PatchU32(data_offset_fields[i], static_cast<uint32_t>(data_offset));
    for (const auto& sample : fragments[i].samples) {
      data_offset += sample.data.size();
    }
  }
  for (size_t i = 0; i < saio_fields.size(); ++i) {
    w.PatchU32(saio_fields[i],
               static_cast<uint32_t>(senc_data_offsets[i] - moof));
  }

  size_t mdat = w.StartBox(FourCC("mdat"));
  for (const auto& fragment : fragments) {
    for (const auto& sample : fragment.samples) {
      w.Bytes(sample.data);
    }
  }
  w.EndBox(mdat);
  return w.Take();
}

std::vector<uint8_t> MakeEsds(
    const std::vector<uint8_t>& audio_specific_config) {
  CHECK_LT(audio_specific_config.size(), 100u);
  uint8_t asc_size = static_cast<uint8_t>(audio_specific_config.size());
  BoxWriter w;
  size_t esds = w.StartFullBox(FourCC("esds"), 0, 0);
  // ES_Descriptor
  w.U8(0x03);
  w.U8(3 + (2 + 13 + 2 + asc_size) + 3);
  w.U16(0);  // ES_ID
  w.U8(0);   // flags
  // DecoderConfigDescriptor
  w.U8(0x04);
  w.U8(13 + 2 + asc_size);
  w.U8(0x40);  // MPEG-4 audio
  w.U8(0x15);  // AudioStream, upstream 0, reserved 1
  w.U24(0);    // bufferSizeDB
  w.U32(0);    // maxBitrate
  w.U32(0);    // avgBitrate
  // DecoderSpecificInfo
  w.U8(0x05);
  w.U8(asc_size);
  w.Bytes(audio_specific_config);
  // SLConfigDescriptor
  w.U8(0x06);
  w.U8(1);
  w.U8(0x02);
  w.EndBox(esds);
  return w.Take();
}

}  // namespace compass::avf::mse
