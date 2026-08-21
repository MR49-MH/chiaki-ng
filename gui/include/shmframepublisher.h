// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_SHM_FRAME_PUBLISHER_H
#define CHIAKI_SHM_FRAME_PUBLISHER_H

#include <QtGlobal>

#include <cstdint>

struct AVFrame;

// Windows-only shared memory frame publisher.
//
// Publishes decoded frames into a named shared memory ring buffer so external
// tools (e.g. mhrs_autotool) can read raw frames without screen capture.
//
// Binary layout must stay in sync with:
//   - mhrs_autotool/src/stream/shm_reader.py
//   - mhrs_autotool/docs/chiaki-shm-integration-plan.md
//
// Shared memory name : Local\chiaki_ng_frames
// Event name         : Local\chiaki_ng_frame_event (auto-reset)
//
// On non-Windows platforms every method is a no-op so call sites need no
// preprocessor guards.
class ShmFramePublisher
{
	public:
		static ShmFramePublisher &instance();

		// Enabling does not allocate; the mapping is created lazily on the
		// first published frame once size/format are known.
		void set_enabled(bool enabled);
		bool enabled() const { return enabled_; }

		// Publish one frame. Accepts hardware frames (a GPU->CPU transfer is
		// performed internally). Never throws; failures are counted only.
		void publish(const AVFrame *frame, int64_t pts_us);

		uint64_t published_frames() const { return published_frames_; }
		uint64_t dropped_frames() const { return dropped_frames_; }

	private:
		ShmFramePublisher() = default;
		~ShmFramePublisher();
		ShmFramePublisher(const ShmFramePublisher &) = delete;
		ShmFramePublisher &operator=(const ShmFramePublisher &) = delete;

		bool configure(uint32_t width, uint32_t height, uint32_t pix_fmt);
		void shutdown();

		bool enabled_ = false;
		bool configured_ = false;
		uint32_t width_ = 0;
		uint32_t height_ = 0;
		uint32_t pix_fmt_ = 0;
		uint32_t plane_count_ = 0;
		uint32_t plane_offset_[4] = {};
		uint32_t plane_stride_[4] = {};
		uint32_t plane_rows_[4] = {};
		uint32_t plane_copy_bytes_[4] = {};
		uint64_t published_frames_ = 0;
		uint64_t dropped_frames_ = 0;

#if defined(Q_OS_WINDOWS)
		void *mapping_handle_ = nullptr;
		void *view_ = nullptr;
		void *event_handle_ = nullptr;
		uint8_t *base_ = nullptr;       // mapped base (256-byte header)
		uint8_t *slots_base_ = nullptr; // base_ + header_size
		uint32_t slot_stride_ = 0;
		uint64_t write_index_ = 0;
#endif
};

#endif // CHIAKI_SHM_FRAME_PUBLISHER_H
