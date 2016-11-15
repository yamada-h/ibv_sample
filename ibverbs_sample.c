#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netdb.h>
#include <infiniband/verbs.h>

#define SIZE (1024*1024)

struct ib_addr {
    uint16_t lid;
    uint8_t port_num;
    uint16_t qp_num;
    uint32_t psn;
};

int main(int argc, char *argv[]) {
  int ret;
  bool is_server = false;
  struct ib_addr my_addr;

  if (argc == 2 && strncmp("-s", argv[1], 2) == 0) {
    printf("Server mode\n");
    is_server = true;
  } else if (argc == 3 && strncmp("-c", argv[1], 2) == 0) {
    printf("Client mode\n");
  } else {
    printf("Usage:\n  ./sample -s\n  ./sample -c <ip address>\n");
    return -1;
  }

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
  my_addr.port_num = 1;
  struct ibv_port_attr port_attr;
  ibv_query_port(ctx, my_addr.port_num, &port_attr);
  printf("LID: %d\n", port_attr.lid);
  my_addr.lid = port_attr.lid;

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
  my_addr.qp_num = qp->qp_num;

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
    .rq_psn = my_addr.psn,
    .max_dest_rd_atomic = 16,
    .min_rnr_timer = 12,
  };
  ret = ibv_modify_qp(qp, &rtr_attr, IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
  if (ret != 0) {
    printf("Error: %d, %s\n", ret, strerror(ret));
    return -1;
  }
  printf("Success: INIT to RTR\n");

  // exchange ib_addr
  struct ib_addr remote_addr;
  if (is_server) {
    int sock_listen = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in sock_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(11122),
      .sin_addr = {
        .s_addr = INADDR_ANY
      }
    };
    bind(sock_listen, (struct sockaddr *)&sock_addr, sizeof(struct sockaddr_in));

    errno = 0;
    ret = listen(sock_listen, 1);
    if (ret != 0) {
      printf("Failed to listen: %s\n", strerror(errno));
      return -1;
    }

    struct sockaddr_in remote_sock_addr;
    socklen_t len = sizeof(struct sockaddr_in);

    printf("Waiting for client...\n");
    int sock = accept(sock_listen, (struct sockaddr *)&remote_sock_addr, &len);

    recv(sock, &remote_addr, sizeof(struct ib_addr), 0);
    printf("Remote: LID=%d, Port=%d, QP=%d\n", remote_addr.lid, remote_addr.port_num, remote_addr.qp_num);
    send(sock, &my_addr, sizeof(struct ib_addr), 0);

    close(sock);
    close(sock_listen);
  }
  else {
    struct addrinfo hints = {
      .ai_socktype = SOCK_STREAM,
      .ai_family = AF_INET,
    };
    struct addrinfo *res;
    ret = getaddrinfo(argv[2], "11122", &hints, &res);
    if (ret != 0) {
      printf("Failed getaddrinfo: %s\n", gai_strerror(ret));
      return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    errno = 0;
    ret = connect(sock, res->ai_addr, res->ai_addrlen);
    if (ret == -1) {
      printf("Failed to connect: %s\n", strerror(errno));
      return -1;
    }

    send(sock, &my_addr, sizeof(struct ib_addr), 0);
    recv(sock, &remote_addr, sizeof(struct ib_addr), 0);
    printf("Remote: LID=%d, Port=%d, QP=%d\n", remote_addr.lid, remote_addr.port_num, remote_addr.qp_num);

    close(sock);
  }

  struct ibv_qp_attr rts_attr = {
    .qp_state = IBV_QPS_RTS,
    .timeout = 14,
    .retry_cnt = 7,
    .rnr_retry = 7,
    .sq_psn = remote_addr.psn,
    .max_rd_atomic = 10,
  };
  ret = ibv_modify_qp(qp, &rts_attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
  if (ret != 0) {
    printf("Error: %d, %s\n", ret, strerror(ret));
    return -1;
  }
  printf("Success: RTR to RTS\n");

  ibv_destroy_qp(qp);
  ibv_dereg_mr(mr);
  ibv_destroy_cq(recv_cq);
  ibv_destroy_cq(send_cq);
  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  ibv_free_device_list(device_list);
}
