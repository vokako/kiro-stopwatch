import socket
import threading
import time

from swlink.protocol import Message
from swlink.tcp import TcpLink


def test_tcp_link_round_trip_and_chunk():
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    received = []

    def peer():
        conn, _ = server.accept()
        data = conn.recv(4096)
        received.append(data)
        # Fragment one JSON event and one mic header/payload across packets.
        conn.sendall(b'{"t":"hello","auth":true')
        conn.sendall(b'}\n{"t":"mic","n":4,"rate":8000,"fmt":"s16le"}\nab')
        conn.sendall(b'cd')
        time.sleep(0.05)
        conn.close()
        server.close()

    threading.Thread(target=peer, daemon=True).start()
    seen: list[Message] = []
    link = TcpLink("127.0.0.1", port, "token", seen.append)
    assert link.open() == f"tcp:127.0.0.1:{port}"
    time.sleep(0.2)
    link.close()

    assert received[0] == b'{"t":"hello","token":"token"}\n'
    assert [(m.t, m.data) for m in seen[:2]] == [("hello", b""), ("mic", b"abcd")]
