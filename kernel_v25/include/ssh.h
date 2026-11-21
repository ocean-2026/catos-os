#ifndef SSH_H
#define SSH_H

/*
 * CatOS v2.71 SSH-2 server (kex: curve25519-sha256, host key ssh-ed25519,
 * cipher aes128-ctr, mac hmac-sha2-256).  Port of the validated host
 * prototype deliverables/ssh-prototype/sshd_min.py -- read its README
 * before touching the packet/H logic.
 *
 * Transport comes from the netstack TCP server (net_tcp_listen/saccept/
 * ssend/srecv*).  The caller (cmd_sshd) accepts connections and calls
 * ssh_serve_session() once per established link.
 */

int  ssh_serve_session(void);
/*   serve one SSH connection end-to-end (KEX + auth + one session
 *   channel).  Returns 0 when the session finished normally, negative
 *   on protocol errors.  Regenerates a fresh host key every call
 *   (clients should connect with StrictHostKeyChecking=no for now). */

#endif
