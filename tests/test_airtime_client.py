import socket
import unittest
from unittest.mock import Mock, patch

from airtime_client import AirtimeClient


class AirtimeClientTests(unittest.TestCase):
    def client(self):
        with patch("airtime_client.socket.socket") as factory:
            client = AirtimeClient()
        self.addCleanup(client.close)
        return client, factory.return_value

    @patch("airtime_client.time.time_ns", return_value=123)
    @patch("airtime_client.time.sleep")
    def test_wait_grant_and_actual_feedback(self, sleep, _clock):
        client, transport = self.client()
        transport.recv.side_effect = [b"WAIT 123 25000", b"GRANT 123", b"DONE 123"]
        transmit = Mock(return_value=800)
        self.assertEqual(client.transmit(500, transmit), 800)
        transmit.assert_called_once_with()
        self.assertEqual([call.args[0] for call in transport.send.call_args_list],
                         [b"ADMIT 123 500", b"ADMIT 123 500", b"REPORT 123 800"])
        sleep.assert_called_once_with(0.025)

    def test_timeout_never_sends_or_retries(self):
        client, transport = self.client()
        transport.recv.side_effect = socket.timeout()
        transmit = Mock()
        with self.assertRaises(socket.timeout):
            client.transmit(500, transmit)
        with self.assertRaises(RuntimeError):
            client.transmit(500, transmit)
        transmit.assert_not_called()
        transport.send.assert_called_once()

    @patch("airtime_client.time.time_ns", return_value=123)
    def test_tx_failure_keeps_gate_reserved(self, _clock):
        client, transport = self.client()
        transport.recv.return_value = b"GRANT 123"
        with self.assertRaises(OSError):
            client.transmit(500, Mock(side_effect=OSError("unknown TX outcome")))
        transport.send.assert_called_once_with(b"ADMIT 123 500")

    def test_invalid_airtime_rejected_before_admission(self):
        client, transport = self.client()
        for value in (0, -1, 1.5, True, 1000001):
            with self.assertRaises(ValueError):
                client.transmit(value, Mock())
        transport.send.assert_not_called()


if __name__ == "__main__":
    unittest.main()
