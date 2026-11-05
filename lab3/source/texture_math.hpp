#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>

namespace application {

inline std::size_t alignedUniformStride(std::size_t size, std::size_t alignment) {
	if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
		throw std::invalid_argument("Uniform size must be positive and alignment must be a power of two");
	if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1))
		throw std::overflow_error("Aligned uniform stride exceeds addressable size");
	return (size + alignment - 1) & ~(alignment - 1);
}

} // namespace application
