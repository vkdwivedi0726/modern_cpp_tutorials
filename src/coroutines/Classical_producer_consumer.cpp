#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <semaphore>
#include <concepts>
#include <chrono>
#include <functional>

// for testing
#include <memory>
#include <iostream>
#include <thread>
#include <iterator>


// CompilerExplorer: https://godbolt.org/z/zYarrTxzK

namespace utils
{ 
    template<typename T, std::size_t BlockSize>
    struct block {
        std::size_t size_; // the size - number of actually stored data
        std::array<T, BlockSize> data_; // the storage, that can hold up to BlockSize elements
    };

    /**
     * Producer-consumer implementation.

     * Relies on storing the memory blocks of the fix size into 
     * preallocated memory with a given number of slots.
     * We use two-semaphores approach, for concurrent 
     * writing/reading into preallocated memory.
     * It's designed to store instead of a single, rather array of elements - most likely
     * bytes (network streaming, file transfer, etc.)
     *
     * @tparam T            Type of the elements to store
     * @tparam Blocks       The number of slots to synchronized with
     * @tparam BlockSize    The size of the each slot, in elements of type T
     */
    template <typename T, std::size_t Blocks, std::size_t BlockSize>
    class RingBuffer final
    {

    public:
    
        using block_type = block<T, BlockSize>;
        
            
        void write(block_type&& data)
        {
            writeSemaphore_.acquire(); // wait on empty slot
            {
                std::lock_guard lock{lock_};
                blocks_[writeIndex_] = std::forward<block_type>(data); 
                writeIndex_ = (writeIndex_ +  1) % Blocks;
            } // unlock
            readSemaphore_.release(); // signal consumer data readiness
        }

        template <typename Collection>
        requires std::convertible_to<decltype(*std::declval<Collection&>().begin()), T>
        std::size_t write(Collection&& collection)
        {
            const auto written = std::min(BlockSize, collection.size());
            writeSemaphore_.acquire();
            {
                std::lock_guard lock{lock_};

                auto&& col = std::forward<Collection>(collection);
                auto& block = blocks_[writeIndex_];
                block.size_ = written;
                std::copy(col.cbegin(), std::next(collection.cbegin(), written), block.data_.begin());
                writeIndex_ = (writeIndex_ + 1) % Blocks;
            } // unlock
            readSemaphore_.release();
            return written;
        }

        bool read(block_type& block) {
            return readImpl(&semaphore_type::acquire, block);
        }

        bool read_for(block_type& block, std::chrono::milliseconds timeout) {
            return readImpl(&semaphore_type::template try_acquire_for<std::uint64_t, std::milli>, block, timeout);
        }

        template <typename Collection>
        auto read(Collection& collection)
        {
            readSemaphore_.acquire();
            return readImpl([&](const block_type& block) mutable
            {
                collection.reserve(block.size_);
                std::copy(block.data_.cbegin(), block.data_.cend(), std::back_inserter(collection));
            });
        }

        template <typename Collection>
        auto read_for(Collection& collection, std::chrono::milliseconds timeout)
        {
            if (not readSemaphore_.try_acquire_for(timeout)) return false;
            return readImpl([&](const block_type& block) mutable
            {
                collection.reserve(block.size_);
                std::copy(block.data_.cbegin(), block.data_.cend(), std::back_inserter(collection));
            });
        }

        // C-style std::span
        // Most likely, the circular buffer will be used for storing the raw bytes
        template <typename Byte>
        static constexpr bool is_byte = std::is_same_v<Byte, unsigned char> || std::is_same_v<Byte, std::uint8_t> || std::is_same_v<Byte, std::byte>;
        bool read_bytes(T* ptr, std::size_t& size) requires is_byte<T>
        {
            readSemaphore_.acquire();
            return readImpl([ptr, &size](const auto& block) mutable
            {
                size = std::min(size, block.size_);
                std::memcpy(ptr, block.data_.data(), size);
            });
        }

        bool read_bytes_for(T* ptr, std::size_t& size, std::chrono::milliseconds timeout) 
        requires is_byte<T>
        {
            if (not readSemaphore_.try_acquire_for(timeout)) return false;
            return readImpl([ptr, &size](const auto& block) mutable
            {
                size = std::min(size, block.size_);
                std::memcpy(ptr, block.data_.data(), size);
            });
        }

        bool is_empty() const 
        {
            std::lock_guard lock {lock_};
            return writeIndex_ == readIndex_;
        }

    private:
        template <typename Func>
        requires std::invocable<Func, block_type>
        bool readImpl(Func&& func) 
        {
            {
               std::lock_guard lock{lock_};
               if ( writeIndex_ == readIndex_) return false;
               std::invoke(std::forward<Func>(func), blocks_[readIndex_]);
            } // unlock
            writeSemaphore_.release();
            return true;
        }
        template <typename Func, typename...Args>  
        bool readImpl(Func&& func, block_type& block, Args&&...args) 
        { 
            if constexpr(std::is_same_v<bool, std::invoke_result_t<Func, semaphore_type, Args...>>) {
                if (not std::invoke(std::forward<Func>(func), readSemaphore_, std::forward<Args>(args)...)) return false;
            }
            else {
                std::invoke(std::forward<Func>(func), readSemaphore_, std::forward<Args>(args)...);
            }
            {
                std::lock_guard lock{lock_};
                if (readIndex_ == writeIndex_) { // empty buffer
                    return false;
                }
                block = blocks_[readIndex_];
                readIndex_ = (readIndex_ + 1) % Blocks;
            } // unlock

            writeSemaphore_.release();
            return true;
        }

    private:
        mutable std::mutex lock_;

        using semaphore_type = std::counting_semaphore<Blocks>;
        semaphore_type writeSemaphore_ {Blocks};
        semaphore_type readSemaphore_ {0};

        std::size_t writeIndex_ = 0; // index of the slot to write to
        std::size_t readIndex_ = 0; // index of the slot to read from

        std::array<block_type, Blocks> blocks_;
    
    }; // RingBuffer
}

// Unit test
namespace test {
    class A final
    {
        public:
            constexpr A(int i) noexcept: m_count(i){}
            constexpr A() = default;

            constexpr operator int() const { return m_count; }
            constexpr int get() const { return m_count; }

        private:
            int m_count = 0;
    };

    template <class Collection>
    requires std::convertible_to<decltype(*std::declval<Collection&>().begin()), A>
    void printA(const Collection& collection, std::size_t size)
    {
        for (std::size_t i = 0; size > i; ++i) {
            std::cout << collection[i] << ' ';
        }
        std::cout << '\n';
    }


    void testRingBuffer()
    {
        using namespace std::chrono_literals;

        using ring_buffer_t = utils::RingBuffer<A, 5, 10>;

        std::shared_ptr<ring_buffer_t> ringBuffer = std::make_shared<ring_buffer_t>();
        std::stop_source stop; // cooperative cancellation mechanism

        auto producer = 
            [&ringBuffer](std::stop_token stopEvent) mutable
            {
                using namespace std::chrono_literals;

                for (;;)
                {
                    if (stopEvent.stop_requested()) break; // polling the stop state

                    // check rvalue
                    std::vector<A> v = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
                    ringBuffer->write(std::move(v));

                    std::this_thread::sleep_for(1s);

                    // check lvalue
                    static constexpr std::array<A, 6> a{11, 12, 13, 14, 15, 16};
                    ringBuffer->write(a);

                    std::this_thread::sleep_for(1s);
                }

                std::cout << "Leaving producer thread\n";
            };

        auto consumer = 
            [&ringBuffer](std::stop_token stopEvent) mutable
            {
                using namespace std::chrono_literals;

                for (;;)
                {
                    ring_buffer_t::block_type data;
                    //if (ringBuffer->read_for(data, 1s)) {printA(data.data_, data.size_);}
                    if (ringBuffer->read(data)) {
                        printA(data.data_, data.size_);
                    }

                    if (stopEvent.stop_requested()) break;

                    std::this_thread::sleep_for(1s);
                }

                std::cout << "Leaving consumer thread\n";
            };
        
        std::jthread consumerThread {consumer, stop.get_token()};
        std::jthread producerThread {producer, stop.get_token()};
        
        std::this_thread::sleep_for(5s);

        stop.request_stop(); // signal cancellation event

    }
}

int main() 
{
    test::testRingBuffer();
}