#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <infiniband/verbs.h>

#define N 1024

int main(int argc, char **argv)
{
    int i, ret;
    char* a = malloc(N);
    int qpnum = atoi(argv[1]);
    ret = ibv_fork_init();
    if (ret) {
        fprintf(stderr, "Failure: ibv_fork_init (errno=%d)\n", ret);
        exit(EXIT_FAILURE);
    }

    struct ibv_device **dev_list;
    dev_list = ibv_get_device_list(NULL);

    if (!dev_list) {
        int errsave = errno;
        fprintf(stderr, "Failure: ibv_get_device_list (errno=%d)\n", errsave);
        exit(EXIT_FAILURE);
    }
    struct ibv_device *device = dev_list[0];

    //User Context
    struct ibv_context *context;
    context = ibv_open_device(device);

    //Protection Domain
    struct ibv_pd *pd;
    pd = ibv_alloc_pd(context);

    //Memory Region
    struct ibv_mr *mr;
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    mr = ibv_reg_mr(pd, a, 1024, access);

    printf("address = %p, r_key = %u \n", a, (uint64_t)mr->rkey);
 
    //create Completion Queue
    struct ibv_cq *cq;
    int cqe = 64;
    void *cq_context=NULL;
    cq = ibv_create_cq(context, cqe, cq_context, NULL, 0);

    //create Queue Pair
    struct ibv_qp *qp;
    struct ibv_qp_init_attr qp_init_attr = {
        .qp_type    = IBV_QPT_RC,
        .qp_context = NULL,
        .send_cq    = cq,
        .recv_cq    = cq,
        .srq        = NULL,
        .cap        = {
            .max_send_wr  = 32,
            .max_recv_wr  = 32,
            .max_send_sge =  1,
            .max_recv_sge =  1,
        },
        .sq_sig_all = 1, 
    };
    qp = ibv_create_qp(pd, &qp_init_attr);


    //check QP Number
    printf("QP Number = %d\n", qp->qp_num);

    //Reset -> Init
    struct ibv_qp_attr init_attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = 1,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE,
    };

    ret = ibv_modify_qp(qp, &init_attr,
                    IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS);

    //Init -> RTR
    struct ibv_qp_attr rtr_attr = {
	.qp_state               = IBV_QPS_RTR,
        .path_mtu               = IBV_MTU_4096,
        .dest_qp_num            = qpnum,
        .rq_psn                 = 1024,
        .max_dest_rd_atomic     = 0,
        .min_rnr_timer          = 0,
        .ah_attr                = {
            .is_global          = 0,
            .dlid               = 16,
            .sl                 = 0,
            .src_path_bits      = 0,
            .port_num           = 1,
        },
   };

   ret = ibv_modify_qp(qp, &rtr_attr,
                    IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER);
   while(1){
   //infinite loop
   }
    ibv_free_device_list(dev_list);

    return 0;
}
