#ifndef _RDMA_UTIL_H_
#define _RDMA_UTIL_H_

#include <infiniband/verbs.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace rdma_util {

struct HandshakeData {
    ibv_gid gid;
    uint16_t lid;
    uint32_t qp_num;
};

void* malloc_gpu_buffer(size_t length, const char* bdf);

enum Result_t {
    SUCCESS = 0,
    DEVICE_NOT_FOUND = 1,
    DEVICE_OPEN_FAILED = 2,
    DEVICE_CLOSE_FAILED = 3,
};

Result_t open_ib_device(ibv_context** ret, const char* dev_name);
Result_t close_ib_device(ibv_context* context);
ibv_qp* create_rc(ibv_context* context);
int bring_up_rc(ibv_qp* qp, HandshakeData& data);
int query_handshake_data(ibv_qp* qp, HandshakeData* handshake_data);

enum QueuePairState {
    RESET = 0,
    INIT = 1,
    RTR = 2,
    RTS = 3,
    UNKNOWN = 4,
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

    Context(const char* dev_name) noexcept(false) {
        auto dev_list = ibv_get_device_list(nullptr);
        if (!dev_list) {
            throw std::runtime_error("Failed to get device list");
        }

        ibv_device* dev = nullptr;
        for (int i = 0; dev_list[i] != nullptr; i++) {
            if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
                dev = dev_list[i];
                break;
            }
        }

        if (dev == nullptr) {
            ibv_free_device_list(dev_list);
            throw std::runtime_error("Device not found");
        }

        auto context = ibv_open_device(dev);
        if (context == nullptr) {
            ibv_free_device_list(dev_list);
            throw std::runtime_error("Failed to open device");
        }

        this->inner = context;
    }

  public:
    inline ibv_context* as_ptr() {
        return this->inner;
    }

    static std::shared_ptr<Context> create(const char* dev_name) noexcept(false) {
        return std::shared_ptr<Context>(new Context(dev_name));
    }

    ~Context() {
        printf("Context destructor\n");
        if (this->inner) {
            ibv_close_device(this->inner);
        }
    }
};

class ProtectionDomain {
    friend class Context;
    friend class MemoryRegion;
    friend class RcQueuePair;

  private:
    ibv_pd* inner;

    std::shared_ptr<Context> context_;

    ProtectionDomain(std::shared_ptr<Context> context) noexcept(false) {
        this->context_ = context;
        this->inner = ibv_alloc_pd(context->inner);
        if (this->inner == nullptr) {
            throw std::runtime_error("Failed to allocate protection domain");
        }
    }

  public:
    static std::shared_ptr<ProtectionDomain> create(std::shared_ptr<Context> context) noexcept(false) {
        return std::shared_ptr<ProtectionDomain>(new ProtectionDomain(context));
    }

    inline std::shared_ptr<Context> get_context() const {
        return this->context_;
    }

    ~ProtectionDomain() {
        printf("ProtectionDomain destructor\n");
        if (this->inner) {
            ibv_dealloc_pd(this->inner);
        }
    }
};

struct WorkCompletion {
    bool success;
    uint64_t wr_id;
};

class RcQueuePair {
    friend class Context;
    friend class MemoryRegion;
    friend class ProtectionDomain;

  private:
    ibv_qp* inner;

    std::shared_ptr<ProtectionDomain> pd_;
    std::shared_ptr<Context> context_;

    RcQueuePair(std::shared_ptr<ProtectionDomain> pd) noexcept(false) {
        this->pd_ = pd;
        this->context_ = pd->context_;
        auto send_cq = ibv_create_cq(context_->as_ptr(), 128, nullptr, nullptr, 0);
        auto recv_cq = ibv_create_cq(context_->as_ptr(), 128, nullptr, nullptr, 0);

        if (send_cq == nullptr || recv_cq == nullptr) {
            if (send_cq) {
                ibv_destroy_cq(send_cq);
            }
            if (recv_cq) {
                ibv_destroy_cq(recv_cq);
            }
            throw std::runtime_error("Failed to create completion queue");
        }

        // ibv_qp_init_attr init_attr {
        //     .send_cq = send_cq,
        //     .recv_cq = recv_cq,
        //     .cap =
        //         {
        //             .max_send_wr = 128,
        //             .max_recv_wr = 1024,
        //             .max_send_sge = 1,
        //             .max_recv_sge = 1,
        //             .max_inline_data = 64,
        //         },
        //     .qp_type = IBV_QPT_RC,
        //     .sq_sig_all = 0,
        // };

        ibv_qp_init_attr init_attr {};
        init_attr.send_cq = send_cq;
        init_attr.recv_cq = recv_cq;
        init_attr.cap.max_send_wr = 128;
        init_attr.cap.max_recv_wr = 1024;
        init_attr.cap.max_send_sge = 1;
        init_attr.cap.max_recv_sge = 1;
        init_attr.cap.max_inline_data = 64;
        init_attr.qp_type = IBV_QPT_RC;
        init_attr.sq_sig_all = 0;

        this->inner = ibv_create_qp(pd->inner, &init_attr);
        if (this->inner == nullptr) {
            ibv_destroy_cq(send_cq);
            ibv_destroy_cq(recv_cq);
            throw std::runtime_error("Failed to create queue pair");
        }
    }

  public:
    static std::shared_ptr<RcQueuePair> create(std::shared_ptr<ProtectionDomain> pd) noexcept(false) {
        return std::shared_ptr<RcQueuePair>(new RcQueuePair(pd));
    }

    static std::shared_ptr<RcQueuePair> create(std::shared_ptr<Context> context) noexcept(false) {
        return std::shared_ptr<RcQueuePair>(new RcQueuePair(ProtectionDomain::create(context)));
    }

    static std::shared_ptr<RcQueuePair> create(const char* dev_name) noexcept(false) {
        return std::shared_ptr<RcQueuePair>(new RcQueuePair(ProtectionDomain::create(Context::create(dev_name))));
    }

    inline std::shared_ptr<ProtectionDomain> get_pd() const {
        return this->pd_;
    }

    inline std::shared_ptr<Context> get_context() const {
        return this->context_;
    }

    QueuePairState query_qp_state() noexcept(false) {
        ibv_qp_attr attr;
        ibv_qp_init_attr init_attr;
        if (ibv_query_qp(this->inner, &attr, IBV_QP_STATE, &init_attr)) {
            throw std::runtime_error("Failed to query QP state");
        }
        switch (attr.qp_state) {
            case ibv_qp_state::IBV_QPS_RESET:
                return QueuePairState::RESET;
            case ibv_qp_state::IBV_QPS_INIT:
                return QueuePairState::INIT;
            case ibv_qp_state::IBV_QPS_RTR:
                return QueuePairState::RTR;
            case ibv_qp_state::IBV_QPS_RTS:
                return QueuePairState::RTS;
            default:
                return QueuePairState::UNKNOWN;
        }
    }

    HandshakeData get_handshake_data() noexcept(false) {
        ibv_gid gid;
        if (ibv_query_gid(this->context_->inner, 1, 0, &gid)) {
            throw std::runtime_error("Failed to query gid");
        }
        ibv_port_attr attr;
        if (ibv_query_port(this->context_->inner, 1, &attr)) {
            throw std::runtime_error("Failed to query port");
        }

        HandshakeData handshake_data {};
        handshake_data.gid = gid;
        handshake_data.lid = attr.lid;
        handshake_data.qp_num = this->inner->qp_num;
        return handshake_data;
    }

    void bring_up(const HandshakeData& handshake_data) noexcept(false) {
        ibv_gid gid = handshake_data.gid;
        uint16_t lid = handshake_data.lid;
        uint32_t remote_qp_num = handshake_data.qp_num;

        {
            // Check QP state
            int mask = ibv_qp_attr_mask::IBV_QP_STATE;
            ibv_qp_attr attr;
            ibv_qp_init_attr init_attr;
            int ret = ibv_query_qp(this->inner, &attr, mask, &init_attr);
            if (ret) {
                throw std::runtime_error("Failed to query QP state");
            } else if (attr.qp_state == ibv_qp_state::IBV_QPS_RTS) {
                printf("[warn] QP state is already RTS\n");
                return;
            } else if (attr.qp_state != ibv_qp_state::IBV_QPS_RESET) {
                throw std::runtime_error("QP state is not RESET");
            }
        }

        {
            // Modify QP to INIT
            int mask = ibv_qp_attr_mask::IBV_QP_STATE | ibv_qp_attr_mask::IBV_QP_PKEY_INDEX
                | ibv_qp_attr_mask::IBV_QP_PORT | ibv_qp_attr_mask::IBV_QP_ACCESS_FLAGS;
            ibv_qp_attr attr = {
                .qp_state = ibv_qp_state::IBV_QPS_INIT,
                .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE,
                .pkey_index = 0,
                .port_num = 1,
            };

            if (ibv_modify_qp(this->inner, &attr, mask)) {
                throw std::runtime_error("Failed to modify to INIT");
            }
        }

        {
            // Modify QP to ready-to-receive
            int mask = ibv_qp_attr_mask::IBV_QP_STATE | ibv_qp_attr_mask::IBV_QP_AV | ibv_qp_attr_mask::IBV_QP_PATH_MTU
                | ibv_qp_attr_mask::IBV_QP_DEST_QPN | ibv_qp_attr_mask::IBV_QP_RQ_PSN
                | ibv_qp_attr_mask::IBV_QP_MAX_DEST_RD_ATOMIC | ibv_qp_attr_mask::IBV_QP_MIN_RNR_TIMER;
            ibv_qp_attr attr = {
                .qp_state = ibv_qp_state::IBV_QPS_RTR,
                .path_mtu = ibv_mtu::IBV_MTU_4096,
                .rq_psn = remote_qp_num,
                .dest_qp_num = remote_qp_num,
                .ah_attr =
                    {
                        .grh {
                            .dgid = gid,
                            .flow_label = 0,
                            .sgid_index = 0,
                            .hop_limit = 255,
                        },
                        .dlid = lid,
                        .is_global = 1,
                        .port_num = 1,
                    },
                .max_dest_rd_atomic = 16,
                .min_rnr_timer = 0,
            };

            if (ibv_modify_qp(this->inner, &attr, mask)) {
                throw std::runtime_error("Failed to modify to RTR");
            }
        }

        {
            // Modify QP to ready-to-send
            int mask = ibv_qp_attr_mask::IBV_QP_STATE | ibv_qp_attr_mask::IBV_QP_TIMEOUT
                | ibv_qp_attr_mask::IBV_QP_RETRY_CNT | ibv_qp_attr_mask::IBV_QP_RNR_RETRY
                | ibv_qp_attr_mask::IBV_QP_SQ_PSN | ibv_qp_attr_mask::IBV_QP_MAX_QP_RD_ATOMIC;

            ibv_qp_attr attr = {
                .qp_state = ibv_qp_state::IBV_QPS_RTS,
                .sq_psn = this->inner->qp_num,
                .max_rd_atomic = 16,
                .timeout = 14,
                .retry_cnt = 7,
                .rnr_retry = 7,
            };

            if (ibv_modify_qp(this->inner, &attr, mask)) {
                throw std::runtime_error("Failed to modify to RTS");
            }
        }
    }

    int post_send(uint64_t wr_id, uint64_t addr, uint32_t length, uint32_t lkey, bool signaled) noexcept {
        ibv_sge sge {};
        sge.addr = addr;
        sge.length = length;
        sge.lkey = lkey;
        ibv_send_wr wr = {};
        wr.wr_id = wr_id;
        wr.next = nullptr;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_SEND;
        wr.send_flags = signaled ? uint32_t(ibv_send_flags::IBV_SEND_SIGNALED) : 0;
        ibv_send_wr* bad_wr;
        return ibv_post_send(this->inner, &wr, &bad_wr);
    }

    int post_recv(uint64_t wr_id, uint64_t addr, uint32_t length, uint32_t lkey) noexcept {
        ibv_sge sge {};
        sge.addr = addr;
        sge.length = length;
        sge.lkey = lkey;
        ibv_recv_wr wr = {};
        wr.wr_id = wr_id;
        wr.next = nullptr;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        ibv_recv_wr* bad_wr;
        return ibv_post_recv(this->inner, &wr, &bad_wr);
    }

    /**
     * @brief poll the send_cq until at least `num_expected_completions` 
     * work completions are polled or an error occurs
     * 
     * @param expected_num_wcs expected number of completions
     * @param polled_wcs return value to store the polled wr_ids and status
     * @return int 0 on success, other values on error. Refer to
     * https://www.rdmamojo.com/2013/02/15/ibv_poll_cq/ for more information.
     */
    int wait_until_send_completion(const int expected_num_wcs, std::vector<WorkCompletion>& polled_wcs) noexcept {
        const int expected = expected_num_wcs;
        ibv_wc* work_completions = new ibv_wc[expected_num_wcs];
        int ret = 0;
        int num_polled_completions = 0;

        if (polled_wcs.size() > 0) {
            polled_wcs.clear();
        }

        while (num_polled_completions < expected_num_wcs) {
            ret = ibv_poll_cq(this->inner->send_cq, expected_num_wcs, work_completions);
            if (ret > 0) {
                printf("ret %d\n", ret);
                for (int i = 0; i < ret; ++i) {
                    WorkCompletion work_completion {};
                    work_completion.success = work_completions[i].status == ibv_wc_status::IBV_WC_SUCCESS;
                    work_completion.wr_id = work_completions[i].wr_id;
                    polled_wcs.push_back(work_completion);
                }
                num_polled_completions += ret;
            } else if (ret < 0) {
                delete[] work_completions;
                return ret;
            }
        }
        delete[] work_completions;
        return 0;
    }

    /**
     * @brief poll the recv_cq until at least `num_expected_completions` 
     * work completions are polled or an error occurs
     * 
     * @param expected_num_wcs expected number of completions
     * @param polled_wcs return value to store the polled wr_ids and status
     * @return int 0 on success, other values on error. Refer to
     * https://www.rdmamojo.com/2013/02/15/ibv_poll_cq/ for more information.
     */
    int wait_until_recv_completion(const int expected_num_wcs, std::vector<WorkCompletion>& polled_wcs) noexcept {
        const int expected = expected_num_wcs;
        ibv_wc* work_completions = new ibv_wc[expected_num_wcs];
        int ret = 0;
        int num_polled_completions = 0;

        if (polled_wcs.size() > 0) {
            polled_wcs.clear();
        }

        while (num_polled_completions < expected_num_wcs) {
            ret = ibv_poll_cq(this->inner->recv_cq, expected_num_wcs, work_completions);
            if (ret > 0) {
                printf("ret %d\n", ret);
                for (int i = 0; i < ret; ++i) {
                    WorkCompletion work_completion {};
                    work_completion.success = work_completions[i].status == ibv_wc_status::IBV_WC_SUCCESS;
                    work_completion.wr_id = work_completions[i].wr_id;
                    polled_wcs.push_back(work_completion);
                }
                num_polled_completions += ret;
            } else if (ret < 0) {
                delete[] work_completions;
                return ret;
            }
        }
        delete[] work_completions;
        return 0;
    }

    ~RcQueuePair() {
        printf("RcQueuePair destructor\n");
        if (this->inner) {
            ibv_destroy_cq(this->inner->send_cq);
            ibv_destroy_cq(this->inner->recv_cq);
            ibv_destroy_qp(this->inner);
        }
    }
};

class MemoryRegion {
    friend class Context;
    friend class ProtectionDomain;
    friend class RcQueuePair;

  private:
    ibv_mr* inner;

    std::shared_ptr<ProtectionDomain> pd_;
    std::shared_ptr<Context> context_;

    MemoryRegion(std::shared_ptr<ProtectionDomain> pd, void* addr, size_t length) noexcept(false) {
        this->pd_ = pd;
        this->context_ = pd->context_;
        this->inner = ibv_reg_mr(
            pd->inner,
            addr,
            length,
            ibv_access_flags::IBV_ACCESS_LOCAL_WRITE | ibv_access_flags::IBV_ACCESS_REMOTE_WRITE
                | ibv_access_flags::IBV_ACCESS_REMOTE_READ
        );
        if (this->inner == nullptr) {
            throw std::runtime_error("Failed to register memory region");
        }
    }

  public:
    static std::shared_ptr<MemoryRegion>
    create(std::shared_ptr<ProtectionDomain> pd, void* addr, size_t length) noexcept(false) {
        return std::shared_ptr<MemoryRegion>(new MemoryRegion(pd, addr, length));
    }

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

    ~MemoryRegion() {
        printf("MemoryRegion destructor\n");
        if (this->inner) {
            ibv_dereg_mr(this->inner);
        }
    }
};

}  // namespace rdma_util

#endif  // _RDMA_UTIL_H_