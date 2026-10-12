#pragma once

// Simple span type since std::span is unavailable in kernel mode

template<typename T> struct kspan {

public:
    constexpr kspan(T* data, size_t size) : m_data(data), m_size(size) {}

    constexpr T* data() const { return m_data; }
    constexpr size_t size() const { return m_size; }

    constexpr T& operator[](size_t index) const { return m_data[index]; }
    constexpr T* begin() const { return m_data; }
    constexpr T* end() const { return m_data + m_size; }

private:
    T* m_data;
    size_t m_size;
};
