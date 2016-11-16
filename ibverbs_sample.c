#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>
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
    uint32_t rkey;
    uint64_t memaddr;
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
    printf("name=%s, guid=0x016%" PRIx64 "\n", ibv_get_device_name(device), ibv_get_device_guid(device));
  }

  struct ibv_device *device = device_list[0];
  struct ibv_context *ctx = ibv_open_device(device);

  struct ibv_device_attr device_attr;
  ibv_query_device(ctx, &device_attr);
  printf("device has %d ports\n", device_attr.phys_port_cnt);
  my_addr.port_num = 1;
  struct ibv_port_attr port_attr;
  ibv_query_port(ctx, my_addr.port_num, &port_attr);
  printf("LID: 0x%04" PRIx16 "\n", port_attr.lid);
  my_addr.lid = port_attr.lid;

  struct ibv_pd *pd = ibv_alloc_pd(ctx);

  struct ibv_cq *send_cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
  struct ibv_cq *recv_cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);

  char *buf = malloc(SIZE);
  struct ibv_mr *mr = ibv_reg_mr(pd, buf, SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  printf("local key=0x%08" PRIx32 ", remote key=0x%08" PRIx32 "\n", mr->lkey, mr->rkey);
  my_addr.memaddr = (uint64_t)buf;
  printf("memory addr=0x%016" PRIx64 "\n", my_addr.memaddr);
  my_addr.rkey = mr->rkey;

  struct ibv_qp_init_attr attr = {
    .send_cq    = send_cq,
    .recv_cq    = recv_cq,
    .qp_type    = IBV_QPT_RC,
    .sq_sig_all = 1,
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
    .port_num = my_addr.port_num,
    .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE,
  };
  ret = ibv_modify_qp(qp, &init_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
  if (ret != 0) {
    printf("Error: %d, %s\n", ret, strerror(ret));
    return -1;
  }
  printf("Success: RESET to INIT\n");

  struct ibv_ah_attr ah_attr = {
    .dlid = port_attr.lid,
    .port_num = my_addr.port_num,
  };
  struct ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
  if (ah == NULL) {
    printf("Failed to create ah\n");
    return -1;
  }

  // exchange ib_addr
  my_addr.psn = 0;
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
    errno = 0;
    ret = bind(sock_listen, (struct sockaddr *)&sock_addr, sizeof(struct sockaddr_in));
    if (ret != 0) {
      printf("Failed to bind: %s\n", strerror(errno));
      return -1;
    }

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

    close(sock);
  }
  printf("Remote: LID=0x%04" PRIx16 ", Port=%d, QP=%d, Memory=0x%016" PRIx64 ", rkey=0x%08" PRIx32 "\n", remote_addr.lid, remote_addr.port_num, remote_addr.qp_num, remote_addr.memaddr, remote_addr.rkey);

  struct ibv_qp_attr rtr_attr = {
    .qp_state = IBV_QPS_RTR,
    .path_mtu = IBV_MTU_4096,
    .ah_attr = ah_attr, 
    .dest_qp_num = remote_addr.qp_num,
    .rq_psn = remote_addr.psn,
    .max_dest_rd_atomic = 16,
    .min_rnr_timer = 12,
  };
  ret = ibv_modify_qp(qp, &rtr_attr, IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
  if (ret != 0) {
    printf("Error: %d, %s\n", ret, strerror(ret));
    return -1;
  }
  printf("Success: INIT to RTR\n");

  struct ibv_qp_attr rts_attr = {
    .qp_state = IBV_QPS_RTS,
    .timeout = 3,
    .retry_cnt = 3,
    .rnr_retry = 7,
    .sq_psn = my_addr.psn,
    .max_rd_atomic = 0,
  };
  ret = ibv_modify_qp(qp, &rts_attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
  if (ret != 0) {
    printf("Error: %d, %s\n", ret, strerror(ret));
    return -1;
  }
  printf("Success: RTR to RTS\n");

  if (is_server) {
    struct ibv_recv_wr recv_wr = {
      .wr_id = 1,
      .next = NULL,
      .sg_list = NULL,
      .num_sge = 0,
    };

    struct ibv_recv_wr *bad_wr;
    ret = ibv_post_recv(qp, &recv_wr, &bad_wr);
    if (ret != 0) {
      printf("Failed to ibv_post_recv: %s\n", strerror(ret));
      return -1;
    }
    printf("Success: Receive Work Request\n");

    struct ibv_wc wc[1];
    int cnt = 0;
    do {
      ret = ibv_poll_cq(recv_cq, 1, wc);
      cnt++;
      if (cnt > 1000000000) {
        printf("polling cq %d times, timeout\n", cnt);
        printf("%s\n", buf);
        /*
        struct ibv_async_event event;
        ret = ibv_get_async_event(ctx, &event);
        printf("Got async event: %d\n", event.event_type);
        ibv_ack_async_event(&event);
        */
        return -1;
      }
    } while (ret == 0);
    printf("polling cq %d times\n", cnt);
    if (ret == -1) {
      printf("Failed to ibv_poll_cq\n");
      return -1;
    }
    printf("Recv RDMA_WRITE with IMM: data=%s, IMM=%x\n", buf, wc[0].imm_data);
  }
  else {
    strncpy(buf, "hogehogefugafuga", 17);
    printf("Send: %s\n", buf);
    struct ibv_sge sge = {
      .addr = (uint64_t)buf,
      .length = SIZE,
      .lkey = mr->lkey,
    };
    struct ibv_send_wr send_wr = {
      .wr_id = 1,
      .next = NULL,
      .sg_list = &sge,
      .num_sge = 1,
      .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
      .imm_data = 0xdeadbeef,
      .wr.rdma = {
        .remote_addr = remote_addr.memaddr,
        .rkey = remote_addr.rkey,
      },
    };

    struct ibv_send_wr *bad_wr;
    ret = ibv_post_send(qp, &send_wr, &bad_wr);
    if (ret != 0) {
      printf("Failed to ibv_post_send: %s\n", strerror(ret));
      return -1;
    }
    printf("Success: Send Work Request\n");

    struct ibv_wc wc[1];
    int cnt = 0;
    do {
      ret = ibv_poll_cq(send_cq, 1, wc);
      cnt++;
      if (cnt > 1000000000) {
        printf("polling cq %d times, timeout\n", cnt);
        return -1;
      }
    } while (ret == 0);
    printf("polling cq %d times\n", cnt);
    if (ret == -1) {
      printf("Failed to ibv_poll_cq\n");
      return -1;
    }
    if (wc[0].status != IBV_WC_SUCCESS) {
      printf("Failed to Send RDMA_WRITE with IMM: status=%d\n", wc[0].status);
      return -1;
    }
  }

  ibv_destroy_qp(qp);
  ibv_dereg_mr(mr);
  ibv_destroy_cq(recv_cq);
  ibv_destroy_cq(send_cq);
  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  ibv_free_device_list(device_list);
}
