#include <stdio.h>
#include <string.h>
#include <infiniband/verbs.h>

#define SIZE (1024*1024)

int main() {
  int ret;

  struct ibv_device **device_list;
  int num_devices;
  device_list = ibv_get_device_list(&num_devices);
  printf("found %d devices\n", num_devices);
  for(int i = 0; i < num_devices; i++) {
    struct ibv_device *device = device_list[i];
    printf("name=%s, guid=%ld\n", ibv_get_device_name(device), ibv_get_device_guid(device));
  }

  struct ibv_device *device = device_list[0];
  struct ibv_context *ctx = ibv_open_device(device);

  struct ibv_device_attr device_attr;
  ibv_query_device(ctx, &device_attr);
  printf("device has %d ports\n", device_attr.phys_port_cnt);
  struct ibv_port_attr port_attr;
  ibv_query_port(ctx, 1, &port_attr);
  printf("LID: %d\n", port_attr.lid);

  struct ibv_pd *pd = ibv_alloc_pd(ctx);

  struct ibv_cq *send_cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
  struct ibv_cq *recv_cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);

  char *buf = malloc(SIZE);
  struct ibv_mr *mr = ibv_reg_mr(pd, buf, SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  printf("local key=%d, remote key=%d\n", mr->lkey, mr->rkey);

  struct ibv_qp_init_attr attr = {
    .send_cq    = send_cq,
    .recv_cq    = recv_cq,
    .qp_type    = IBV_QPT_RC,
  };
  struct ibv_qp *qp = ibv_create_qp(pd, &attr);
  if (qp == NULL) {
    printf("Failed to create queue pair\n");
    return -1;
  }
  printf("qpnum=%d, max_send_wr=%d\n", qp->qp_num, attr.cap.max_send_wr);

  struct ibv_qp_attr init_attr = {
    .qp_state = IBV_QPS_INIT,
    .pkey_index = 0,
    .port_num = 1,
    .qp_access_flags = IBV_ACCESS_REMOTE_WRITE,
  };
  ret = ibv_modify_qp(qp, &init_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
  if (ret != 0) {
    printf("Error: %d, %s\n", ret, strerror(ret));
    return -1;
  }
  printf("Success: RESET to INIT\n");

  struct ibv_ah_attr ah_attr = {
    .dlid = port_attr.lid,
    .port_num = 1,
  };
  struct ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
  if (ah == NULL) {
    printf("Failed to create ah\n");
    return -1;
  }

  struct ibv_qp_attr rtr_attr = {
    .qp_state = IBV_QPS_RTR,
    .path_mtu = IBV_MTU_512,
    .ah_attr = ah_attr, 
    .dest_qp_num = 592,
    .rq_psn = 0,
    .max_dest_rd_atomic = 16,
    .min_rnr_timer = 12,
  };
  ret = ibv_modify_qp(qp, &rtr_attr, IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
  if (ret != 0) {
    printf("Error: %d, %s\n", ret, strerror(ret));
    return -1;
  }
  printf("Success: INIT to RTR\n");

  ibv_destroy_qp(qp);
  ibv_dereg_mr(mr);
  ibv_destroy_cq(recv_cq);
  ibv_destroy_cq(send_cq);
  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  ibv_free_device_list(device_list);
}
