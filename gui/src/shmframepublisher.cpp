// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "shmframepublisher.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <QDebug>
#include <cstring>

#ifdef Q_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <new>
#endif

// ---------------------------------------------------------------------------
// Shared layout (must match mhrs_autotool/src/stream/shm_reader.py)
// ---------------------------------------------------------------------------

#ifdef Q_OS_WINDOWS

constexpr uint32_t SHM_MAGIC = 0x46534843u; // 'CHSF' little-endian
constexpr uint32_t SHM_VERSION = 1;
constexpr uint32_t HEADER_SIZE = 256;
constexpr uint32_t SLOT_META_SIZE = 64;
constexpr uint32_t SLOT_COUNT = 8;
constexpr wchar_t MAPPING_NAME[] = L"Local\\chiaki_ng_frames";
constexpr wchar_t EVENT_NAME[] = L"Local\\chiaki_ng_frame_event";

constexpr uint32_t PIX_FMT_CODE_NV12 = 0;
constexpr uint32_t PIX_FMT_CODE_YUV420P = 1;

struct ShmHeader
{
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t slot_stride;
	uint32_t width;
	uint32_t height;
	uint32_t pix_fmt;
	uint32_t plane_count;
	uint32_t plane_offset[4];
	uint32_t plane_stride[4];
	uint32_t slot_size;      // valid bytes per slot (meta + planes)
	uint32_t reserved0;
	uint64_t seq;            // seqlock: odd while writing, even when stable
	uint64_t write_index;    // newest slot index
	uint64_t frame_counter;  // total published frames
	int64_t pts_us;          // mirror of newest slot meta
	int64_t qpc_write_us;    // mirror of newest slot meta
	uint8_t reserved1[256 - 120];
};

struct ShmSlotMeta
{
	uint64_t frame_id;
	int64_t pts_us;
	int64_t qpc_write_us;
	uint32_t width;
	uint32_t height;
	uint32_t pix_fmt;
	uint32_t reserved[7];
};

static_assert(sizeof(ShmHeader) == 256, "ShmHeader size must be 256");
static_assert(offsetof(ShmHeader, seq) == 72, "seq offset must match shm_reader.py");
static_assert(offsetof(ShmHeader, write_index) == 80, "write_index offset must match shm_reader.py");
static_assert(sizeof(ShmSlotMeta) == 64, "ShmSlotMeta size must be 64");

static int64_t qpc_now_us()
{
	static const int64_t freq = []() {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return static_cast<int64_t>(f.QuadPart);
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return c.QuadPart * 1000000LL / freq;
}

#endif // Q_OS_WINDOWS

// ---------------------------------------------------------------------------

ShmFramePublisher &ShmFramePublisher::instance()
{
	static ShmFramePublisher publisher;
	return publisher;
}

void ShmFramePublisher::set_enabled(bool enabled)
{
	if(enabled_ == enabled)
		return;
	enabled_ = enabled;
#ifdef Q_OS_WINDOWS
	if(!enabled_)
		shutdown();
	qInfo() << "ShmFramePublisher:" << (enabled_ ? "enabled" : "disabled");
#endif
}

#ifdef Q_OS_WINDOWS

bool ShmFramePublisher::configure(uint32_t width, uint32_t height, uint32_t pix_fmt)
{
	switch(pix_fmt)
	{
		case PIX_FMT_CODE_NV12:
			plane_count_ = 2;
			plane_offset_[0] = 0;
			plane_offset_[1] = width * height;
			plane_stride_[0] = width;
			plane_stride_[1] = width;
			plane_rows_[0] = height;
			plane_rows_[1] = height / 2;
			plane_copy_bytes_[0] = width;
			plane_copy_bytes_[1] = width;
			break;
		case PIX_FMT_CODE_YUV420P:
			plane_count_ = 3;
			plane_offset_[0] = 0;
			plane_offset_[1] = width * height;
			plane_offset_[2] = width * height + width * height / 4;
			plane_stride_[0] = width;
			plane_stride_[1] = width / 2;
			plane_stride_[2] = width / 2;
			plane_rows_[0] = height;
			plane_rows_[1] = height / 2;
			plane_rows_[2] = height / 2;
			plane_copy_bytes_[0] = width;
			plane_copy_bytes_[1] = width / 2;
			plane_copy_bytes_[2] = width / 2;
			break;
		default:
			return false;
	}

	const uint32_t data_size = width * height * 3 / 2;
	slot_stride_ = SLOT_META_SIZE + data_size;
	const uint32_t map_size = HEADER_SIZE + SLOT_COUNT * slot_stride_;

	mapping_handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, map_size, MAPPING_NAME);
	if(!mapping_handle_)
	{
		qWarning() << "ShmFramePublisher: CreateFileMappingW failed" << GetLastError();
		return false;
	}
	view_ = MapViewOfFile(mapping_handle_, FILE_MAP_ALL_ACCESS, 0, 0, map_size);
	if(!view_)
	{
		qWarning() << "ShmFramePublisher: MapViewOfFile failed" << GetLastError();
		CloseHandle(mapping_handle_);
		mapping_handle_ = nullptr;
		return false;
	}
	base_ = static_cast<uint8_t *>(view_);
	slots_base_ = base_ + HEADER_SIZE;

	memset(base_, 0, HEADER_SIZE);
	auto *hdr = reinterpret_cast<ShmHeader *>(base_);
	hdr->magic = SHM_MAGIC;
	hdr->version = SHM_VERSION;
	hdr->header_size = HEADER_SIZE;
	hdr->slot_stride = slot_stride_;
	hdr->width = width;
	hdr->height = height;
	hdr->pix_fmt = pix_fmt;
	hdr->plane_count = plane_count_;
	for(uint32_t i = 0; i < 4; ++i)
	{
		hdr->plane_offset[i] = plane_offset_[i];
		hdr->plane_stride[i] = plane_stride_[i];
	}
	hdr->slot_size = SLOT_META_SIZE + data_size;
	new (&hdr->seq) std::atomic<uint64_t>{0}; // seqlock starts even at 0
	hdr->write_index = 0;
	write_index_ = 0;

	event_handle_ = CreateEventW(nullptr, FALSE, FALSE, EVENT_NAME);
	if(!event_handle_)
	{
		qWarning() << "ShmFramePublisher: CreateEventW failed" << GetLastError();
		shutdown();
		return false;
	}

	width_ = width;
	height_ = height;
	pix_fmt_ = pix_fmt;
	configured_ = true;
	qInfo() << "ShmFramePublisher: publishing" << width << "x" << height << "fmt" << pix_fmt
			<< "map_bytes" << map_size;
	return true;
}

void ShmFramePublisher::shutdown()
{
	configured_ = false;
	if(view_)
		UnmapViewOfFile(view_);
	if(mapping_handle_)
		CloseHandle(mapping_handle_);
	if(event_handle_)
		CloseHandle(event_handle_);
	view_ = nullptr;
	mapping_handle_ = nullptr;
	event_handle_ = nullptr;
	base_ = nullptr;
	slots_base_ = nullptr;
	slot_stride_ = 0;
}

void ShmFramePublisher::publish(const AVFrame *frame, int64_t pts_us)
{
	if(!enabled_ || !frame)
		return;

	const AVFrame *src = frame;
	AVFrame *sw_tmp = nullptr;
	if(frame->hw_frames_ctx) // zero-copy direct render path: frame lives in GPU memory
	{
		sw_tmp = av_frame_alloc();
		if(!sw_tmp)
		{
			++dropped_frames_;
			return;
		}
		if(av_hwframe_transfer_data(sw_tmp, const_cast<AVFrame *>(frame), 0) < 0)
		{
			qWarning() << "ShmFramePublisher: av_hwframe_transfer_data failed";
			av_frame_free(&sw_tmp);
			++dropped_frames_;
			return;
		}
		av_frame_copy_props(sw_tmp, frame);
		src = sw_tmp;
	}

	uint32_t fmt_code = UINT32_MAX;
	if(src->format == AV_PIX_FMT_NV12)
		fmt_code = PIX_FMT_CODE_NV12;
	else if(src->format == AV_PIX_FMT_YUV420P)
		fmt_code = PIX_FMT_CODE_YUV420P;

	if(fmt_code == UINT32_MAX || src->width <= 0 || src->height <= 0)
	{
		++dropped_frames_;
		av_frame_free(&sw_tmp);
		return;
	}

	if(!configured_ || width_ != static_cast<uint32_t>(src->width) ||
		height_ != static_cast<uint32_t>(src->height) || pix_fmt_ != fmt_code)
	{
		shutdown();
		if(!configure(static_cast<uint32_t>(src->width), static_cast<uint32_t>(src->height), fmt_code))
		{
			++dropped_frames_;
			av_frame_free(&sw_tmp);
			return;
		}
	}

	auto *hdr = reinterpret_cast<ShmHeader *>(base_);
	auto *seq = reinterpret_cast<std::atomic<uint64_t> *>(&hdr->seq);
	const uint64_t slot_index = write_index_ % SLOT_COUNT;
	uint8_t *slot = slots_base_ + slot_index * slot_stride_;

	seq->fetch_add(1, std::memory_order_relaxed); // odd: writing

	for(uint32_t p = 0; p < plane_count_; ++p)
	{
		const uint8_t *sp = src->data[p];
		uint8_t *dp = slot + SLOT_META_SIZE + plane_offset_[p];
		for(uint32_t r = 0; r < plane_rows_[p]; ++r)
			memcpy(dp + r * plane_stride_[p], sp + r * static_cast<uint32_t>(src->linesize[p]),
				plane_copy_bytes_[p]);
	}

	const uint64_t frame_id = ++published_frames_;
	auto *meta = reinterpret_cast<ShmSlotMeta *>(slot);
	meta->frame_id = frame_id;
	meta->pts_us = pts_us;
	meta->qpc_write_us = qpc_now_us();
	meta->width = width_;
	meta->height = height_;
	meta->pix_fmt = pix_fmt_;

	hdr->write_index = slot_index;
	hdr->frame_counter = frame_id;
	hdr->pts_us = pts_us;
	hdr->qpc_write_us = meta->qpc_write_us;

	seq->fetch_add(1, std::memory_order_release); // even: stable
	++write_index_;

	if(event_handle_)
		SetEvent(event_handle_);

	av_frame_free(&sw_tmp);
}

ShmFramePublisher::~ShmFramePublisher()
{
	shutdown();
}

#else // !Q_OS_WINDOWS — no-op stubs so call sites need no guards

bool ShmFramePublisher::configure(uint32_t, uint32_t, uint32_t) { return false; }
void ShmFramePublisher::shutdown() {}
void ShmFramePublisher::publish(const AVFrame *, int64_t) { ++dropped_frames_; }
ShmFramePublisher::~ShmFramePublisher() {}

#endif
