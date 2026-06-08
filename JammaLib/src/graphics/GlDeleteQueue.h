#pragma once

#include <mutex>
#include <thread>
#include <vector>
#include <gl/glew.h>

namespace graphics::GlDeleteQueue
{
	enum class DeleteKind
	{
		Buffers,
		VertexArrays,
		Textures,
		Programs,
		Framebuffers,
		Renderbuffers
	};

	struct PendingDelete
	{
		DeleteKind Kind;
		std::vector<GLuint> Ids;
	};

	inline std::mutex& _Mutex() noexcept
	{
		static std::mutex m;
		return m;
	}

	inline std::thread::id& _RenderThreadId() noexcept
	{
		static std::thread::id id;
		return id;
	}

	inline std::vector<PendingDelete>& _Queue() noexcept
	{
		static std::vector<PendingDelete> q;
		return q;
	}

	inline void SetRenderThread(std::thread::id id) noexcept
	{
		std::lock_guard<std::mutex> lock(_Mutex());
		_RenderThreadId() = id;
	}

	inline bool IsRenderThread() noexcept
	{
		std::lock_guard<std::mutex> lock(_Mutex());
		auto renderThread = _RenderThreadId();
		if (renderThread == std::thread::id())
			return false;

		return std::this_thread::get_id() == renderThread;
	}

	inline std::vector<GLuint> _FilterIds(GLsizei n, const GLuint* ids)
	{
		std::vector<GLuint> filtered;
		if ((n <= 0) || (ids == nullptr))
			return filtered;

		filtered.reserve(static_cast<size_t>(n));
		for (GLsizei i = 0; i < n; ++i)
		{
			if (ids[i] != 0)
				filtered.push_back(ids[i]);
		}

		return filtered;
	}

	inline void _DeleteNow(DeleteKind kind, const std::vector<GLuint>& ids)
	{
		if (ids.empty())
			return;

		switch (kind)
		{
		case DeleteKind::Buffers:
			glDeleteBuffers(static_cast<GLsizei>(ids.size()), ids.data());
			break;
		case DeleteKind::VertexArrays:
			glDeleteVertexArrays(static_cast<GLsizei>(ids.size()), ids.data());
			break;
		case DeleteKind::Textures:
			glDeleteTextures(static_cast<GLsizei>(ids.size()), ids.data());
			break;
		case DeleteKind::Programs:
			for (auto id : ids)
				glDeleteProgram(id);
			break;
		case DeleteKind::Framebuffers:
			glDeleteFramebuffers(static_cast<GLsizei>(ids.size()), ids.data());
			break;
		case DeleteKind::Renderbuffers:
			glDeleteRenderbuffers(static_cast<GLsizei>(ids.size()), ids.data());
			break;
		}
	}

	inline void _DeleteOrQueue(DeleteKind kind, GLsizei n, const GLuint* ids)
	{
		auto filtered = _FilterIds(n, ids);
		if (filtered.empty())
			return;

		bool runNow = false;
		{
			std::lock_guard<std::mutex> lock(_Mutex());
			auto renderThread = _RenderThreadId();
			runNow = (renderThread != std::thread::id()) && (renderThread == std::this_thread::get_id());
			if (!runNow)
			{
				_Queue().push_back(PendingDelete{ kind, std::move(filtered) });
				return;
			}
		}

		_DeleteNow(kind, filtered);
	}

	inline void DeleteBuffers(GLsizei n, const GLuint* ids)
	{
		_DeleteOrQueue(DeleteKind::Buffers, n, ids);
	}

	inline void DeleteVertexArrays(GLsizei n, const GLuint* ids)
	{
		_DeleteOrQueue(DeleteKind::VertexArrays, n, ids);
	}

	inline void DeleteTextures(GLsizei n, const GLuint* ids)
	{
		_DeleteOrQueue(DeleteKind::Textures, n, ids);
	}

	inline void DeleteProgram(GLuint id)
	{
		_DeleteOrQueue(DeleteKind::Programs, 1, &id);
	}

	inline void DeleteFramebuffers(GLsizei n, const GLuint* ids)
	{
		_DeleteOrQueue(DeleteKind::Framebuffers, n, ids);
	}

	inline void DeleteRenderbuffers(GLsizei n, const GLuint* ids)
	{
		_DeleteOrQueue(DeleteKind::Renderbuffers, n, ids);
	}

	inline void FlushPendingDeletes()
	{
		std::vector<PendingDelete> pending;
		{
			std::lock_guard<std::mutex> lock(_Mutex());
			auto renderThread = _RenderThreadId();
			if ((renderThread == std::thread::id()) || (renderThread != std::this_thread::get_id()))
				return;

			pending.swap(_Queue());
		}

		for (auto& item : pending)
			_DeleteNow(item.Kind, item.Ids);
	}
}