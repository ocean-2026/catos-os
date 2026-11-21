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
 *   on protocol errors.  Host key is persistent (/etc/ssh_host.key)
 *   since v2.72 -- known_hosts entries stay valid across sessions. */

/* v2.72: ring0 daemon entry (task_spawn_r0).  Waits for the NIC, listens
 * on 22 and serves sessions forever; never returns.  Started by `sshd -d`
 * or by the /etc/autoexec boot script line "@sshd". */
void sshd_daemon_main(void);

#endif
