#include "core/Buffer.h"

#include <cstring>
#include <iostream>

#include <audio/AudioBuffer.h>

template std::list<coro::audio::AudioBuffer> coro::core::Buffer::split(size_t) const;

namespace coro {
namespace core {

Buffer::Buffer(size_t size)
{
    m_buffer.resize(size);
    m_size = 0;
}

Buffer::Buffer(const uint8_t* data, size_t size, size_t reservedSize)
{
    m_buffer.resize(std::max(size, reservedSize));
    m_size = size;
    std::memcpy(m_buffer.data(), data, size);
}

Buffer::~Buffer()
{
}

uint8_t* Buffer::data()
{
    return m_buffer.data()+m_offset;
}

size_t Buffer::size() const
{
    return m_size;
}

uint8_t* Buffer::acquire(size_t size)
{
    // If we have space in front
    if (m_offset >= size) {
        return m_buffer.data();
    }
    // If we have space at back
    const auto sizeAtBack = m_buffer.size()-m_offset-m_size;
    if (sizeAtBack >= size) {
        m_acquiredOffset = m_offset+m_size;
        return m_buffer.data()+m_acquiredOffset;
    }
    // Create space
    m_buffer.resize(m_buffer.size()-sizeAtBack+size);
    m_acquiredOffset = m_offset+m_size;
    return m_buffer.data()+m_acquiredOffset;
}

void Buffer::commit(size_t size)
{
    m_offset = m_acquiredOffset;
    m_size = size;
}

template <class T>
std::list<T> Buffer::split(size_t size) const
{
    std::list<T> buffers;
    for (size_t i = 0; i < m_size; i += size) {
        buffers.emplace_back(T(m_buffer.data()+m_offset+i, size));
    }
    return buffers;
}

} // namespace core
} // namespace coro
