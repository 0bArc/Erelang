#pragma once
#include <functional>
#include <mutex>
#include <vector>

namespace erelang {

class GC {
public:
	using Finalizer = std::function<void()>;

	GC() = default;

	void register_finalizer(Finalizer finalizer);
	void collect() noexcept;
	void clear() noexcept;

private:
	std::mutex mutex_;
	std::vector<Finalizer> finalizers_;
};

} // namespace erelang
