#!/usr/bin/env python3
import socket

# Backend pool – adjust IPs if your topology differs
servers = [
    ("192.168.0.101", 80),
    ("192.168.0.102", 80),
    ("192.168.0.103", 80),
]

def read_n(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return data
        data += chunk
    return data

def main():
    # Set up listening socket on 0.0.0.0:80
    listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listen.bind(("0.0.0.0", 80))
    listen.listen(128)
    print("[LB] Listening on 0.0.0.0:80")

    rr_index = 0
    while True:
        client, addr = listen.accept()
        try:
            req = read_n(client, 2)
            if len(req) != 2:
                client.close()
                continue

            # pick next server
            host, port = servers[rr_index]
            rr_index = (rr_index + 1) % len(servers)

            # forward request
            serv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            serv.connect((host, port))
            serv.sendall(req)

            # read response (up to 4 KB)
            resp = serv.recv(4096)
            if resp:
                client.sendall(resp)

            serv.close()
        except Exception as e:
            print(f"[LB] Error: {e}")
        finally:
            client.close()

if __name__ == "__main__":
    main()
