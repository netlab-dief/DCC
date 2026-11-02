"""Client for the local DCC gate. All services on one radio must use this gate.

The callback must wait for actual TX completion and return PHY airtime in us,
including any retransmissions. A successful send()/enqueue is not completion.
"""
import socket
import threading
import time


class AirtimeClient:
    def __init__(self, address=("127.0.0.1", 5556), timeout=1.0):
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.connect(address)
        self._socket.settimeout(timeout)
        self._lock = threading.Lock()
        self._last_id = 0
        self._failed = False

    def close(self):
        self._socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def _exchange(self, command, request_id, airtime):
        self._socket.send(f"{command} {request_id} {airtime}".encode("ascii"))
        response = self._socket.recv(256).decode("ascii").split()
        if response[0] == "ERROR":
            raise RuntimeError("DCC gate: " + " ".join(response))
        if len(response) < 2 or response[1] != str(request_id):
            raise RuntimeError("Unexpected DCC gate response")
        return response

    def transmit(self, estimated_airtime_us, send_and_wait):
        """Wait for admission, transmit once, then report actual airtime.

        No automatic retry after a timeout: the daemon may have granted a
        reservation. Exceptions leave the gate closed until operator recovery.
        """
        if type(estimated_airtime_us) is not int or not 1 <= estimated_airtime_us <= 1000000:
            raise ValueError("airtime must be integer microseconds in [1, 1000000]")
        with self._lock:
            if self._failed:
                raise RuntimeError("Uncertain TX state: client disabled; inspect transmitter and gate")
            request_id = max(time.time_ns(), self._last_id + 1)
            self._last_id = request_id
            try:
                while True:
                    reply = self._exchange("ADMIT", request_id, estimated_airtime_us)
                    if reply == ["GRANT", str(request_id)]:
                        break
                    if len(reply) != 3 or reply[0] != "WAIT":
                        raise RuntimeError("Unexpected admission response")
                    time.sleep(min(max(int(reply[2]) / 1000000.0, 0.001), 0.1))
                actual = send_and_wait()
                if type(actual) is not int or not 1 <= actual <= 1000000:
                    raise ValueError("TX callback must return measured PHY airtime in microseconds")
                if self._exchange("REPORT", request_id, actual) != ["DONE", str(request_id)]:
                    raise RuntimeError("Unexpected completion response")
                return actual
            except BaseException:
                self._failed = True
                raise
