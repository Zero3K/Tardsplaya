// TX-Queue wrapper header for Tardsplaya
// This header properly includes tx-queue with correct paths

#pragma once

// Include the original tx-queue header but fix the .inl include path
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <tuple>
#include <string>
#include <type_traits>
#include <thread>

#pragma warning(push)
#pragma warning(disable : 4324 4625 5026 4626 5027)

#define QCS_DECLARE_QUEUE_FRIENDS \
    template<typename QTYPE>      \
    friend class tx_write_t;      \
    template<typename QTYPE>      \
    friend class tx_read_t;

namespace qcstudio {

    using namespace std;

    // Use a compile-time constant for cache line size since std::hardware_destructive_interference_size
    // is not always a constant expression in MSVC
    constexpr size_t CACHE_LINE_SIZE = 64;

    class base_tx_queue_t {
    public:
        auto     is_ok() const -> bool;
        auto     capacity() const -> uint64_t;
        explicit operator bool() const noexcept;

    protected:
        alignas(CACHE_LINE_SIZE) uint8_t* storage_ = nullptr;
        uint64_t capacity_                         = 0;
        QCS_DECLARE_QUEUE_FRIENDS
    };

    struct tx_queue_status_t {
        alignas(CACHE_LINE_SIZE) uint64_t tail_;
        int32_t producer_core_ = -1;
        alignas(CACHE_LINE_SIZE) uint64_t head_;
        int32_t consumer_core_ = -1;
    };

    class tx_queue_sp_t : public base_tx_queue_t {
    public:
        tx_queue_sp_t(uint64_t _capacity);
        ~tx_queue_sp_t();

    private:
        tx_queue_status_t status_;
        QCS_DECLARE_QUEUE_FRIENDS
    };

    class tx_queue_mp_t : public base_tx_queue_t {
    public:
        tx_queue_mp_t(uint8_t* _prealloc_and_init, uint64_t _capacity);

    private:
        QCS_DECLARE_QUEUE_FRIENDS
        tx_queue_status_t& status_;
    };

    template<typename QTYPE>
    class alignas(CACHE_LINE_SIZE) tx_write_t {
    public:
        tx_write_t(QTYPE& _queue);
        ~tx_write_t();
        explicit operator bool() const noexcept;

        auto write(const void* _buffer, uint64_t _size) -> bool;
        template<typename T>                       auto write(const T& _item) -> bool;
        template<typename T, uint64_t N>           auto write(const T (&_array)[N]) -> bool;
        template<typename FIRST, typename... REST> auto write(const FIRST& _first, REST... _rest) -> typename std::enable_if<!std::is_pointer<FIRST>::value, bool>::type;

        void invalidate();

    private:
        QTYPE&   queue_;
        uint8_t* storage_;
        uint64_t tail_, cached_head_, capacity_;
        bool     invalidated_ : 1;

        auto imp_write(const void* _buffer, uint64_t _size) -> bool;
    };

    template<typename QTYPE>
    class alignas(CACHE_LINE_SIZE) tx_read_t {
    public:
        tx_read_t(QTYPE& _queue);
        ~tx_read_t();
        explicit operator bool() const noexcept;

        auto read(void* _buffer, uint64_t _size) -> bool;
        template<typename T>      auto read(T& _item) -> bool;
        template<typename...ARGS> auto read() -> std::tuple<ARGS...>;

        void invalidate();

    private:
        QTYPE&   queue_;
        uint8_t* storage_;
        uint64_t head_, cached_tail_, capacity_;
        bool     invalidated_ : 1;

        auto imp_read(void* _buffer, uint64_t _size) -> bool;
    };

}  // namespace qcstudio

// Include implementation manually to avoid path issues
#include "tx-queue-impl.inl"

#pragma warning(pop)