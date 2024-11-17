#ifndef _RDMA_UTIL_H_
#define _RDMA_UTIL_H_

#include <infiniband/verbs.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "concurrentqueue.h"

typedef int (*mem_cpy_fp)(void*, void*, uint64_t);

namespace rdma_util {

struct HandshakeData {
    ibv_gid gid;
    uint16_t lid;
    uint32_t qp_num;
};

enum QueuePairState {
    RESET = 0,
    INIT = 1,
    RTR = 2,
    RTS = 3,
    UNKNOWN = 4,
};

struct WorkCompletion {
    uint64_t wr_id;

    // See ibv_wc_status for detailed meaining of status
    uint32_t status;

    uint32_t byte_len;

    // See ibv_wc_status for detailed meaining of status
    uint32_t opcode;
    uint32_t imm_data;

    inline std::string to_string() const {
        return "wr_id: " + std::to_string(wr_id) + ", status: " + std::to_string(status)
            + ", byte_len: " + std::to_string(byte_len) + ", opcode: " + std::to_string(opcode)
            + ", imm_data: " + std::to_string(imm_data);
    }
};

class Context;
class ProtectionDomain;
class RcQueuePair;
class MemoryRegion;

class Context {
    friend class ProtectionDomain;
    friend class MemoryRegion;
    friend class RcQueuePair;

  private:
    ibv_context* inner;

    Context(const char* dev_name) noexcept(false);

  public:
    Context() = delete;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    ~Context();

    static std::unique_ptr<Context> create(const char* dev_name) noexcept(false);
};

class ProtectionDomain {
    friend class Context;
    friend class MemoryRegion;
    friend class RcQueuePair;

  private:
    ibv_pd* inner;

    std::shared_ptr<Context> context_;

    ProtectionDomain(std::shared_ptr<Context> context) noexcept(false);

  public:
    ProtectionDomain() = delete;
    ProtectionDomain(const ProtectionDomain&) = delete;
    ProtectionDomain& operator=(const ProtectionDomain&) = delete;

    ~ProtectionDomain();

    static std::unique_ptr<ProtectionDomain> create(std::shared_ptr<Context> context) noexcept(false);

    inline std::shared_ptr<Context> get_context() const {
        return this->context_;
    }
};

class RcQueuePair {
    friend class Context;
    friend class MemoryRegion;
    friend class ProtectionDomain;

  private:
    ibv_qp* inner;

    std::shared_ptr<ProtectionDomain> pd_;
    std::shared_ptr<Context> context_;

    RcQueuePair(std::shared_ptr<ProtectionDomain> pd) noexcept(false);

  public:
    RcQueuePair() = delete;
    RcQueuePair(const RcQueuePair&) = delete;
    RcQueuePair& operator=(const RcQueuePair&) = delete;

    ~RcQueuePair();

    static std::unique_ptr<RcQueuePair> create(const char* dev_name) noexcept(false);
    static std::unique_ptr<RcQueuePair> create(std::shared_ptr<Context> context) noexcept(false);
    static std::unique_ptr<RcQueuePair> create(std::shared_ptr<ProtectionDomain> pd) noexcept(false);

    inline std::shared_ptr<ProtectionDomain> get_pd() const {
        return this->pd_;
    }

    inline std::shared_ptr<Context> get_context() const {
        return this->context_;
    }

    QueuePairState query_qp_state() noexcept(false);

    HandshakeData get_handshake_data() noexcept(false);

    void bring_up(const HandshakeData& handshake_data) noexcept(false);

    int post_send_send(uint64_t wr_id, uint64_t laddr, uint32_t length, uint32_t lkey, bool signaled) noexcept;

    int post_send_send_with_imm(
        uint64_t wr_id,
        uint64_t laddr,
        uint32_t length,
        uint32_t lkey,
        uint32_t imm,
        bool signaled
    ) noexcept;

    int post_send_read(
        uint64_t wr_id,
        uint64_t laddr,
        uint64_t raddr,
        uint32_t length,
        uint32_t lkey,
        uint32_t rkey,
        bool signaled
    ) noexcept;

    int post_send_write(
        uint64_t wr_id,
        uint64_t laddr,
        uint64_t raddr,
        uint32_t length,
        uint32_t lkey,
        uint32_t rkey,
        bool signaled
    ) noexcept;

    int post_send_write_with_imm(
        uint64_t wr_id,
        uint64_t laddr,
        uint64_t raddr,
        uint32_t length,
        uint32_t imm,
        uint32_t lkey,
        uint32_t rkey,
        bool signaled
    ) noexcept;

    int post_recv(uint64_t wr_id, uint64_t addr, uint32_t length, uint32_t lkey) noexcept;

    /**
     * @brief poll the send_cq until at least `num_expected_completions` 
     * work completions are polled or an error occurs
     * 
     * @param expected_num_wcs expected number of completions
     * @param polled_wcs return value to store the polled wr_ids and status
     * @return int 0 on success, other values on error. Refer to
     * https://www.rdmamojo.com/2013/02/15/ibv_poll_cq/ for more information.
     */
    int wait_until_send_completion(const int expected_num_wcs, std::vector<WorkCompletion>& polled_wcs) noexcept;

    /**
     * @brief poll the recv_cq until at least `num_expected_completions` 
     * work completions are polled or an error occurs
     * 
     * @param expected_num_wcs expected number of completions
     * @param polled_wcs return value to store the polled wr_ids and status
     * @return int 0 on success, other values on error. Refer to
     * https://www.rdmamojo.com/2013/02/15/ibv_poll_cq/ for more information.
     */
    int wait_until_recv_completion(const int expected_num_wcs, std::vector<WorkCompletion>& polled_wcs) noexcept;

    /**
     * @brief poll the send_cq once and return the number of polled work completions on success
     * 
     * @param max_num_wcs maximum number of work completions to poll
     * @param polled_wcs return value to store the polled wr_ids and status
     */
    int poll_send_cq_once(const int max_num_wcs, std::vector<WorkCompletion>& polled_wcs) noexcept;

    /**
     * @brief poll the recv_cq once and return the number of polled work completions on success
     * 
     * @param max_num_wcs maximum number of work completions to poll
     * @param polled_wcs return value to store the polled wr_ids and status
     */
    int poll_recv_cq_once(const int max_num_wcs, std::vector<WorkCompletion>& polled_wcs) noexcept;

    /**
     * UNSAFE: This function panics if the size of `wc_buffer` is less than the sizeof `ibv_wc[max_num_wcs]`
     * @brief poll the send_cq once and return the number of polled work completions on success
     * 
     * @param max_num_wcs maximum number of work completions to poll
     * @param wc_buffer work completion buffer. You must ensure that the buffer is larger than the sizeof `ibv_wc[max_num_wcs]`
     * @param polled_wcs return value to store the polled wr_ids and status
     */
    int poll_send_cq_once(const int max_num_wcs, ibv_wc* wc_buffer, std::vector<WorkCompletion>& polled_wcs);

    /**
     * UNSAFE: This function panics if the size of `wc_buffer` is less than the sizeof `ibv_wc[max_num_wcs]`
     * @brief poll the recv_cq once and return the number of polled work completions on success
     * 
     * @param max_num_wcs maximum number of work completions to poll
     * @param wc_buffer work completion buffer. You must ensure that the buffer is larger than the sizeof `ibv_wc[max_num_wcs]`
     * @param polled_wcs return value to store the polled wr_ids and status
     */
    int poll_recv_cq_once(const int max_num_wcs, ibv_wc* wc_buffer, std::vector<WorkCompletion>& polled_wcs);
};

class MemoryRegion {
    friend class Context;
    friend class ProtectionDomain;
    friend class RcQueuePair;

  private:
    ibv_mr* inner;

    std::shared_ptr<ProtectionDomain> pd_;
    std::shared_ptr<Context> context_;

    // This could be nullptr if the buffer is created with a raw pointer
    std::shared_ptr<void> inner_buffer_with_deleter_;

    MemoryRegion(
        std::shared_ptr<ProtectionDomain> pd,
        std::shared_ptr<void> buffer_with_deleter,
        uint64_t length
    ) noexcept(false);

    MemoryRegion(std::shared_ptr<ProtectionDomain> pd, void* addr, uint64_t length) noexcept(false);

  public:
    MemoryRegion() = delete;
    MemoryRegion(const MemoryRegion&) = delete;
    MemoryRegion& operator=(const MemoryRegion&) = delete;

    ~MemoryRegion();

    /**
     * @brief Create a MemoryRegion object with a buffer that has a deleter
     * 
     * SAFETY: The caller must ensure that the buffer is valid and it has a deleter that handles the buffer's deallocation
     * 
     * @param pd ProtectionDomain object
     * @param buffer_with_deleter buffer with a deleter
     * @param length length of the buffer
     */
    static std::unique_ptr<MemoryRegion>
    create(std::shared_ptr<ProtectionDomain> pd, std::shared_ptr<void> buffer_with_deleter, uint64_t length) noexcept(
        false
    );

    /**
     * @brief Create a MemoryRegion object with a buffer that has a deleter
     * 
     * SAFETY: The caller must ensure that the buffer has longer lifetime than the returned MemoryRegion object
     * 
     * @param pd ProtectionDomain object
     * @param addr address of the raw buffer
     * @param length length of the buffer
     */
    static std::unique_ptr<MemoryRegion>
    create(std::shared_ptr<ProtectionDomain> pd, void* addr, uint64_t length) noexcept(false);

    inline uint32_t get_lkey() const {
        return this->inner->lkey;
    }

    inline uint32_t get_rkey() const {
        return this->inner->rkey;
    }

    inline void* get_addr() const {
        return this->inner->addr;
    }

    inline uint64_t get_length() const {
        return this->inner->length;
    }
};

template<typename T>
using Queue = std::shared_ptr<moodycamel::ConcurrentQueue<T>>;

struct Ticket {
    uint32_t stream_id;
    uint32_t length;
    uint64_t addr;
    uint32_t key;

    inline std::string to_string() const {
        return "stream_id: " + std::to_string(stream_id) + ", length: " + std::to_string(length)
            + ", addr: " + std::to_string(addr) + ", key: " + std::to_string(key);
    }
};

using Command = std::tuple<Ticket, std::shared_ptr<std::atomic<int>>>;

template<typename T>
using MultiMap = std::map<uint32_t, std::queue<T>>;

enum TcclContextAPI {
    // Using post_send_write_with_imm
    V1,
    // Using post_send_send
    V2,
};

class TcclContext {
  private:
    Queue<Ticket> send_request_command_queue_;
    Queue<Command> recv_request_command_queue_;

    // Backgound worker threads
    std::thread thread_post_send_;
    std::thread thread_post_recv_;

    std::shared_ptr<std::atomic<bool>> finalized_;

    // API
    TcclContextAPI api_version_;

    TcclContext() = delete;
    TcclContext(const TcclContext&) = delete;
    TcclContext& operator=(const TcclContext&) = delete;

    // V1 constructor
    TcclContext(std::unique_ptr<RcQueuePair> qp) noexcept(false);

    void initialize_v1(std::unique_ptr<RcQueuePair> qp) noexcept(false);

    static void thread_post_send_v1(
        std::shared_ptr<RcQueuePair> qp,
        std::unique_ptr<MemoryRegion> host_send_buffer,
        std::shared_ptr<std::atomic<bool>> finalized,
        Queue<Ticket> local_send_request_queue,
        Queue<Ticket> local_recv_request_queue,
        Queue<Ticket> remote_recv_request_queue
    ) noexcept(false);

    static void thread_post_recv_v1(
        std::shared_ptr<RcQueuePair> qp,
        std::unique_ptr<MemoryRegion> host_recv_buffer,
        std::shared_ptr<std::atomic<bool>> finalized,
        Queue<Command> recv_command_queue,
        Queue<Ticket> local_recv_request_queue,
        Queue<Ticket> remote_recv_request_queue
    ) noexcept(false);

    // V2 constructtor
    TcclContext(
        std::unique_ptr<RcQueuePair> qp,
        std::shared_ptr<MemoryRegion> device_send_buffer,
        std::shared_ptr<MemoryRegion> device_recv_buffer,
        mem_cpy_fp mem_cpy_func
    ) noexcept(false);

    // V2
    void initialize_v2(
        std::unique_ptr<RcQueuePair> qp,
        std::shared_ptr<MemoryRegion> device_send_buffer,
        std::shared_ptr<MemoryRegion> device_recv_buffer,
        mem_cpy_fp mem_cpy_func
    ) noexcept(false);

    static void thread_post_send_v2(
        std::shared_ptr<RcQueuePair> qp,
        std::shared_ptr<MemoryRegion> device_send_buffer,
        std::shared_ptr<std::atomic<bool>> finalized,
        mem_cpy_fp mem_cpy_func,
        Queue<Ticket> local_send_request_queue
    ) noexcept(false);

    static void thread_post_recv_v2(
        std::shared_ptr<RcQueuePair> qp,
        std::shared_ptr<MemoryRegion> device_recv_buffer,
        std::shared_ptr<std::atomic<bool>> finalized,
        mem_cpy_fp mem_cpy_func,
        Queue<Command> recv_command_queue
    ) noexcept(false);

  public:
    static std::shared_ptr<TcclContext> create_v1(std::unique_ptr<RcQueuePair> qp) noexcept(false);
    void send_v1(uint32_t stream_id, uint64_t addr, uint32_t length, uint32_t lkey);
    void recv_v1(uint32_t stream_id, uint64_t addr, uint32_t length, uint32_t rkey);

    [[deprecated("V2 API is unrecommended. Use create_v1 instead.")]]
    static std::shared_ptr<TcclContext> create_v2(
        std::unique_ptr<RcQueuePair> qp,
        std::shared_ptr<MemoryRegion> device_send_buffer,
        std::shared_ptr<MemoryRegion> device_recv_buffer,
        mem_cpy_fp mem_cpy_func
    ) noexcept(false);

    [[deprecated("V2 API is unrecommended. Use send_v1 instead.")]]
    void send_v2(uint32_t stream_id, uint64_t addr, uint32_t length);

    [[deprecated("V2 API is unrecommended. Use recv_v1 instead.")]]
    void recv_v2(uint32_t stream_id, uint64_t addr, uint32_t length);

    ~TcclContext();
};

}  // namespace rdma_util

#endif  // _RDMA_UTIL_H_