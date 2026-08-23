// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "shmframepublisher.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
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
constexpr wchar_t MUTEX_NAME[] = L"Local\\chiaki_ng_frames_lock";

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
	uint32_t publisher_pid;  // pid of the writing process (0 = legacy writer)
	uint8_t reserved1[256 - 116];
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
	// 先除后乘：uptime×freq×1e6 会在开机约 10.7 天(10MHz QPC)时溢出 int64，
	// 导致 qpc_write_us 回绕成负数、读取端新鲜度判断永远失败。
	return c.QuadPart / freq * 1000000LL + c.QuadPart % freq * 1000000LL / freq;
}

// Serializes mapping creation/adoption between processes: without it, two
// instances starting simultaneously could both pass the stale-detection gate
// (the loser reads the header in the microseconds before the winner writes
// its pid) and both publish into the same section.
struct NamedMutexGuard
{
	HANDLE handle = nullptr;
	bool owned = false;
	explicit NamedMutexGuard(bool &ok)
	{
		ok = false;
		handle = CreateMutexW(nullptr, FALSE, MUTEX_NAME);
		if(!handle)
			return;
		// WAIT_ABANDONED also means we own the mutex now (previous holder
		// died while holding it) — fine: whatever it left behind is either
		// absent or stale, and the adoption gate validates that anyway.
		const DWORD wait = WaitForSingleObject(handle, 1000);
		if(wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
			return;
		owned = true;
		ok = true;
	}
	~NamedMutexGuard()
	{
		if(!handle)
			return;
		if(owned)
			ReleaseMutex(handle);
		CloseHandle(handle);
	}
	NamedMutexGuard(const NamedMutexGuard &) = delete;
	NamedMutexGuard &operator=(const NamedMutexGuard &) = delete;
};

#endif // Q_OS_WINDOWS

// ---------------------------------------------------------------------------

ShmFramePublisher &ShmFramePublisher::instance()
{
	static ShmFramePublisher publisher;
	return publisher;
}

void ShmFramePublisher::set_enabled(bool enabled)
{
#ifdef Q_OS_WINDOWS
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if(enabled_.load(std::memory_order_relaxed) == enabled)
			return;
		enabled_.store(enabled, std::memory_order_relaxed);
		if(enabled_)
		{
			// (Re)spawn the worker; a previous stop() joined it.
			thread_exit_ = false;
			if(!publisher_thread_.joinable())
				publisher_thread_ = std::thread(&ShmFramePublisher::PublisherLoop, this);
		}
	}
	if(!enabled_)
		StopWorkerAndShutdown();
	qInfo() << "ShmFramePublisher:" << (enabled ? "enabled" : "disabled");
#endif
}

void ShmFramePublisher::publish(const AVFrame *frame, int64_t pts_us)
{
#ifdef Q_OS_WINDOWS
	if(!enabled_.load(std::memory_order_relaxed) || !frame)
		return;

	// Producer side only: take a new reference (frames from
	// avcodec_receive_frame are refcounted, hw frames included) and hand it to
	// the worker. av_hwframe_transfer_data and the plane copies run off this
	// thread, so the decode->present chain never waits on them.
	AVFrame *ref = av_frame_clone(const_cast<AVFrame *>(frame));
	if(!ref)
	{
		++dropped_frames_;
		return;
	}

	bool enqueue_failed = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if(!enabled_.load(std::memory_order_relaxed) || thread_exit_)
		{
			enqueue_failed = true;
		}
		else
		{
			while(queue_.size() >= QUEUE_MAX)
			{
				// Reader can't keep up (or worker is busy): drop the OLDEST
				// pending frame, never block the decode thread.
				av_frame_free(&queue_.front().frame);
				queue_.pop_front();
				++dropped_frames_;
			}
			queue_.push_back({ref, pts_us});
			queue_cv_.notify_one();
		}
	}
	if(enqueue_failed)
	{
		av_frame_free(&ref);
		++dropped_frames_;
	}
#else
	++dropped_frames_;
#endif
}

#ifdef Q_OS_WINDOWS

bool ShmFramePublisher::configure_locked(uint32_t width, uint32_t height, uint32_t pix_fmt)
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

	bool locked = false;
	NamedMutexGuard cross_process_lock(locked);
	if(!locked)
	{
		qWarning() << "ShmFramePublisher: could not acquire" << MUTEX_NAME
				   << "within 1s, refusing to configure";
		return false;
	}

	mapping_handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, map_size, MAPPING_NAME);
	if(!mapping_handle_)
	{
		qWarning() << "ShmFramePublisher: CreateFileMappingW failed" << GetLastError();
		return false;
	}
	// A named section from a previous run can still exist (and keep its name
	// occupied) even though the old chiaki process is long gone: any external
	// reader that still holds a mapped view keeps the kernel object alive.
	// Capture existence BEFORE any other call clobbers LastError.
	const bool already_existed = GetLastError() == ERROR_ALREADY_EXISTS;

	// Windows keeps an existing section's original size regardless of what we
	// request here, and mapping more than that fails outright. Map the whole
	// object (size 0) and verify its real size ourselves below.
	view_ = MapViewOfFile(mapping_handle_, FILE_MAP_ALL_ACCESS, 0, 0, already_existed ? 0 : map_size);
	if(!view_)
	{
		qWarning() << "ShmFramePublisher: MapViewOfFile failed" << GetLastError();
		CloseHandle(mapping_handle_);
		mapping_handle_ = nullptr;
		return false;
	}
	base_ = static_cast<uint8_t *>(view_);
	slots_base_ = base_ + HEADER_SIZE;

	if(already_existed)
	{
		// Decide whether the leftover section may be adopted. Requirements:
		//   1. big enough for our geometry (VirtualQuery reports the real
		//      section size; writing through a smaller object would crash);
		//   2. no live publisher: the header records the writing process id,
		//      and a live, different pid means a genuine second instance;
		//   3. actually idle: no successful write in the last 3s, which also
		//      catches legacy publishers that recorded no pid.
		MEMORY_BASIC_INFORMATION mbi{};
		const bool size_known = VirtualQuery(view_, &mbi, sizeof(mbi)) != 0;
		const size_t actual_size = size_known ? mbi.RegionSize : 0;
		const bool size_ok = size_known && actual_size >= map_size;

		const auto *hdr = reinterpret_cast<const ShmHeader *>(base_);
		const bool magic_ok = hdr->magic == SHM_MAGIC && hdr->version == SHM_VERSION;
		const DWORD prev_pid = hdr->publisher_pid;
		bool prev_alive = false;
		if(prev_pid && prev_pid != GetCurrentProcessId())
		{
			if(HANDLE prev = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, prev_pid))
			{
				CloseHandle(prev);
				prev_alive = true;
			}
		}
		const int64_t idle_us = qpc_now_us() - hdr->qpc_write_us;
		const bool idle = !magic_ok || idle_us < 0 || idle_us > 3000000LL;

		if(!(size_ok && !prev_alive && idle))
		{
			qWarning() << "ShmFramePublisher: mapping" << MAPPING_NAME
					   << "already exists and is not adoptable (actual_bytes"
					   << actual_size << "needed" << map_size << "| prev_pid"
					   << prev_pid << "alive" << prev_alive << "| idle_s"
					   << idle_us / 1000000LL << ") refusing";
			shutdown_locked();
			return false;
		}
		qWarning() << "ShmFramePublisher: adopting stale mapping" << MAPPING_NAME
				   << "(prev_pid" << prev_pid << "idle_s" << idle_us / 1000000LL
				   << "actual_bytes" << actual_size << ")";
	}

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
	hdr->publisher_pid = GetCurrentProcessId();
	new (&hdr->seq) std::atomic<uint64_t>{0}; // seqlock starts even at 0
	hdr->write_index = 0;
	write_index_ = 0;

	event_handle_ = CreateEventW(nullptr, FALSE, FALSE, EVENT_NAME);
	if(!event_handle_)
	{
		qWarning() << "ShmFramePublisher: CreateEventW failed" << GetLastError();
		shutdown_locked();
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

void ShmFramePublisher::shutdown_locked()
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

void ShmFramePublisher::StopWorkerAndShutdown()
{
#ifdef Q_OS_WINDOWS
	{
		std::lock_guard<std::mutex> lock(mutex_);
		enabled_.store(false, std::memory_order_relaxed);
		while(!queue_.empty())
		{
			av_frame_free(&queue_.front().frame);
			queue_.pop_front();
		}
		thread_exit_ = true;
		queue_cv_.notify_all();
	}
	// Join WITHOUT holding mutex_: the worker needs mutex_ one last time to
	// observe thread_exit_ before it can exit.
	if(publisher_thread_.joinable())
		publisher_thread_.join();

	std::lock_guard<std::mutex> lock(mutex_);
	shutdown_locked();
#endif
}

void ShmFramePublisher::PublisherLoop()
{
#ifdef Q_OS_WINDOWS
	for(;;)
	{
		QueuedFrame qf {nullptr, 0};
		{
			std::unique_lock<std::mutex> lock(mutex_);
			queue_cv_.wait(lock, [&] { return thread_exit_ || !queue_.empty(); });
			if(thread_exit_)
				return;
			qf = queue_.front();
			queue_.pop_front();
		}
		PublishSync(qf.frame, qf.pts_us);
		av_frame_free(&qf.frame);
	}
#endif
}

void ShmFramePublisher::PublishSync(const AVFrame *frame, int64_t pts_us)
{
#ifdef Q_OS_WINDOWS
	// GPU->CPU transfer runs outside the lock: it can take milliseconds and
	// must not stall a concurrent set_enabled(false) on the GUI thread.
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
			const uint64_t now = qpc_now_us();
			if(now - last_transfer_fail_qpc_us_ >= 5000000LL) // throttle: don't spam at frame rate
			{
				qWarning() << "ShmFramePublisher: av_hwframe_transfer_data failed";
				last_transfer_fail_qpc_us_ = now;
			}
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

	bool geometry_bad = fmt_code == UINT32_MAX || src->width <= 0 || src->height <= 0;
	if(!geometry_bad)
	{
		const uint32_t planes = (fmt_code == PIX_FMT_CODE_NV12) ? 2u : 3u;
		for(uint32_t p = 0; p < planes; ++p)
		{
			// a negative stride would walk out of the buffer backwards
			if(!src->data[p] || src->linesize[p] <= 0)
			{
				geometry_bad = true;
				break;
			}
		}
	}
	if(geometry_bad)
	{
		++dropped_frames_;
		av_frame_free(&sw_tmp);
		return;
	}

	std::lock_guard<std::mutex> lock(mutex_);

	// Session may have quit while we were transferring from GPU memory.
	if(!enabled_.load(std::memory_order_relaxed))
	{
		av_frame_free(&sw_tmp);
		return;
	}

	const uint32_t w = static_cast<uint32_t>(src->width);
	const uint32_t h = static_cast<uint32_t>(src->height);
	if(!configured_ || width_ != w || height_ != h || pix_fmt_ != fmt_code)
	{
		// Don't retry a failing geometry at frame rate: wait for the geometry
		// to change or a 5s cooldown (a stale external reader holding the
		// previous named section may disconnect in the meantime).
		const bool failed_before_same = last_fail_fmt_ == fmt_code && last_fail_w_ == w && last_fail_h_ == h;
		const bool cooldown_over = qpc_now_us() - last_fail_qpc_us_ >= 5000000LL;
		if(failed_before_same && !cooldown_over)
		{
			++dropped_frames_;
			av_frame_free(&sw_tmp);
			return;
		}
		shutdown_locked();
		if(!configure_locked(w, h, fmt_code))
		{
			last_fail_w_ = w;
			last_fail_h_ = h;
			last_fail_fmt_ = fmt_code;
			last_fail_qpc_us_ = qpc_now_us();
			++dropped_frames_;
			av_frame_free(&sw_tmp);
			return;
		}
		last_fail_fmt_ = UINT32_MAX;
	}

	auto *hdr = reinterpret_cast<ShmHeader *>(base_);
	auto *seq = reinterpret_cast<std::atomic<uint64_t> *>(&hdr->seq);
	const uint64_t slot_index = write_index_ % SLOT_COUNT;
	uint8_t *slot = slots_base_ + slot_index * slot_stride_;

	seq->fetch_add(1, std::memory_order_release); // odd: writing

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
#endif
}

ShmFramePublisher::~ShmFramePublisher()
{
#ifdef Q_OS_WINDOWS
	StopWorkerAndShutdown();
#endif
}

#else // !Q_OS_WINDOWS — no-op stubs so call sites need no guards
// publish() is defined above for all platforms (it no-ops when not enabled).

bool ShmFramePublisher::configure_locked(uint32_t, uint32_t, uint32_t) { return false; }
void ShmFramePublisher::shutdown_locked() {}
ShmFramePublisher::~ShmFramePublisher() {}

#endif
