#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <infiniband/verbs.h>

#define N 1024

int main(int argc, char **argv)
{
   struct pdata		*server pdata; 
   struct rdma_event channel	*cm_channel; 
   struct rdma_cm_id	*cm_id; 
   struct rdma_cm_event		*event;  
   struct rdma_conn_param	conn_param = { };  
   struct ibv_pd	*pd; 
   struct ibv_comp_channel	*comp_chan; 
   struct ibv_cq	*cq; 
   struct ibv_cq	*evt_cq; 
   struct ibv_mr	*mr; 
   struct ibv_qp_init_attr	qp attr = { }; 
   struct ibv_sge	sge; 
   struct ibv_send_wr	send_wr = { }; 
   struct ibv_send_wr 	*bad send wr; 
   struct ibv_recv_wr	recv wr = { }; 
   struct ibv_recv_wr	*bad recv wr; 
   struct ibv_wc	wc; 
   void			*cq context; 
   struct addrinfo	*res, *t; 
    int i, ret;
    char *a = malloc(N);
    int qpnum = atoi(argv[1]);
    ret = ibv_fork_init();
    if (ret) {
        fprintf(stderr, "Failure: ibv_fork_init (errno=%d)\n", ret);
        exit(EXIT_FAILURE);
    }



    cm_channel = rdma_create_event_channel(); 
    if (!cm_channel){  return 1; }
 
    err = rdma_create_id(cm_channel, &cm_id, NULL, RDMA_PS_TCP);
    if (err){  return err; }

    n = getaddrinfo(argv[1], "20079", &hints, &res);
    if (n < 0){ return   1; }

    /* Resolve server address and route */
    for (t = res; t; t = t->ai next) {
    	err = rdma_resolve_addr(cm_id, NULL, t->ai_addr, RESOLVE_TIMEOUT_MS);
    	if (!err){ break; }
    }
    if (err)
	return err;
    err = rdma_get_cm_event(cm_channel, &event); 
    if (err)
 	return err;
    if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED)
	return 1;
    rdma_ack_cm_event(event);
    err = rdma_resolve_route(cm_id, RESOLVE_TIMEOUT_MS);
    if (err)
	return err;
    err = rdma_get_cm_event(cm_channel, &event);
    if (err)
	return err;
    if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED)
	return 1;
    rdma_ack_cm_event(event);

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
    int access = IBV_ACCESS_LOCAL_WRITE;
    mr = ibv_reg_mr(pd, (void *)a, 1024,  access);
    strcpy(a, "Hello");

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
        .srq        = NULL, /*Don't use SRQ */
        .cap        = {
            .max_send_wr  = 32,
            .max_recv_wr  = 32,
            .max_send_sge =  1,
            .max_recv_sge =  1,
        },
        .sq_sig_all = 1, 
    };
    qp = ibv_create_qp(pd, &qp_init_attr);


/*
    //check QP Number & destroy qp
    printf("QP Number = %d\n", qp->qp_num);
    int qd = ibv_destroy_qp(qp);
    printf("destroy is %d\n", qd);
*/
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
        .dest_qp_num 		= qpnum,
        .rq_psn                 = 0,
        .max_dest_rd_atomic     = 0,
        .min_rnr_timer          = 0,
        .ah_attr                = {
            .is_global          = 0,
            .dlid               = 17,
            .sl                 = 0,
            .src_path_bits      = 0,
            .port_num           = 1,
        },
   };

   ret = ibv_modify_qp(qp, &rtr_attr,
                    IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER);


   //RTR -> RTS
   struct ibv_qp_attr rts_attr = {
        .qp_state           = IBV_QPS_RTS,
        .timeout            = 0,
        .retry_cnt          = 7,
        .rnr_retry          = 7,
        .sq_psn             = 1024,
        .max_rd_atomic      = 0,
    };

    ret = ibv_modify_qp(qp, &rts_attr,
                    IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC);

    printf("qpnum = %d\n", qp->qp_num);




   //Send Work Request
   struct ibv_sge sge = {
       .addr   = (uint64_t)a,
       .length = 1024,
       .lkey   = mr->lkey,
   };
   struct ibv_send_wr send_wr = {
       .wr_id      = (uint64_t)(uintptr_t)sge.addr,
       .next       = NULL,
       .sg_list    = &sge,
       .num_sge    = 1,
       .opcode     = IBV_WR_RDMA_WRITE,
       .send_flags = 0,
       .wr.rdma.remote_addr = 0x10ca3a0,
       .wr.rdma.rkey = 2147551506
   };

   struct ibv_send_wr *bad_wr;

   ret = ibv_post_send(qp, &send_wr, &bad_wr);
   
   //Polling CQ
   struct ibv_wc wc;

   retry:
   ret = ibv_poll_cq(cq, 1, &wc);
   if (ret == 0){
       goto retry; /* polling */
   }

   if (ret < 0) {
       fprintf(stderr, "Failure: ibv_poll_cq\n");
       exit(EXIT_FAILURE);
   }
    printf("ret : %d, wc : %d\n", ret, wc.status);
   /*     
   if (wc.status != IBV_WC_SUCCESS) {
       fprintf(stderr, "Completion errror\n");
       exit(EXIT_FAILURE);
   }
   */
   switch (wc.opcode) {
       case IBV_WC_SEND:
           goto retry;
       case IBV_WC_RECV:
           printf("Success: wr_id=%016" PRIx64 " byte_len=%u, imm_data=%x\n", wc.wr_id, wc.byte_len, wc.imm_data);
           break;
       case IBV_WC_RDMA_WRITE:
	   printf("Success RDMA Write\n");
	   break;
       default:
          exit(EXIT_FAILURE);
   }


    ibv_free_device_list(dev_list);

    return 0;
}
