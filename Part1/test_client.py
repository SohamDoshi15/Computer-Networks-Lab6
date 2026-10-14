import hashlib
import os
import socket
import struct
import sys

VERSION = 1

GET = 1
MANIFEST = 2
FILE_START = 3
DATA = 4
FILE_END = 5
ACK = 6
ERROR = 7
COMPLETE = 8

HEADER = struct.Struct("!BBHQ16s")


def recv_exact(sock, size):
    data = bytearray()

    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("Server disconnected unexpectedly")
        data.extend(chunk)

    return bytes(data)


def send_frame(sock, msg_type, transfer_id, payload=b""):
    header = HEADER.pack(
        VERSION, msg_type, 0, len(payload), transfer_id
    )
    sock.sendall(header + payload)


def recv_frame(sock):
    raw = recv_exact(sock, HEADER.size)
    version, msg_type, flags, length, transfer_id = HEADER.unpack(raw)

    if version != VERSION:
        raise ValueError("Unsupported protocol version")

    if length > 64 * 1024 * 1024:
        raise ValueError("Frame exceeds payload limit")

    payload = recv_exact(sock, length) if length else b""
    return msg_type, transfer_id, payload


def read_string(data, offset):
    length = struct.unpack_from("!I", data, offset)[0]
    offset += 4
    value = data[offset:offset + length].decode("utf-8")
    return value, offset + length


def main():
    if len(sys.argv) != 4:
        print("Usage: python3 test_client.py <server_ip> <port> <topic>")
        sys.exit(1)

    server_ip = sys.argv[1]
    port = int(sys.argv[2])
    topic = sys.argv[3]

    transfer_id = bytes(16)
    output_dir = os.path.join("test_output", topic)
    os.makedirs(output_dir, exist_ok=True)

    with socket.create_connection((server_ip, port), timeout=10) as sock:
        # Request a topic.
        topic_bytes = topic.encode("utf-8")
        send_frame(
            sock,
            GET,
            transfer_id,
            struct.pack("!I", len(topic_bytes)) + topic_bytes
        )

        msg_type, response_id, payload = recv_frame(sock)

        if msg_type == ERROR:
            code = struct.unpack_from("!I", payload, 0)[0]
            message, _ = read_string(payload, 4)
            raise RuntimeError(f"Server error {code}: {message}")

        if msg_type != MANIFEST:
            raise RuntimeError(f"Expected MANIFEST, received type {msg_type}")

        if response_id != transfer_id:
            raise RuntimeError("Transfer ID mismatch")

        count = struct.unpack_from("!I", payload, 0)[0]
        offset = 4
        manifest = []

        for _ in range(count):
            index = struct.unpack_from("!I", payload, offset)[0]
            offset += 4

            name, offset = read_string(payload, offset)

            size = struct.unpack_from("!Q", payload, offset)[0]
            offset += 8

            digest = payload[offset:offset + 32]
            offset += 32

            manifest.append((index, name, size, digest))

        if offset != len(payload):
            raise RuntimeError("Malformed manifest")

        print(f"Topic: {topic}")
        print(f"Manifest contains {count} files")

        for index, name, size, digest in manifest:
            print(f"  {name}: {size} bytes, SHA-256 {digest.hex()}")

        received_files = 0
        total_bytes = 0

        for index, name, expected_size, expected_hash in manifest:
            msg_type, response_id, payload = recv_frame(sock)

            if msg_type == ERROR:
                raise RuntimeError(f"Server error: {payload!r}")

            if msg_type != FILE_START:
                raise RuntimeError(f"Expected FILE_START, got {msg_type}")

            start_index = struct.unpack_from("!I", payload, 0)[0]
            start_name, pos = read_string(payload, 4)
            start_size = struct.unpack_from("!Q", payload, pos)[0]
            pos += 8
            start_hash = payload[pos:pos + 32]

            if (
                start_index != index
                or start_name != name
                or start_size != expected_size
                or start_hash != expected_hash
            ):
                raise RuntimeError("FILE_START does not match manifest")

            if os.path.basename(name) != name or name in (".", ".."):
                raise RuntimeError("Unsafe filename received")

            temp_path = os.path.join(output_dir, name + ".part")
            final_path = os.path.join(output_dir, name)

            file_hash = hashlib.sha256()
            file_size = 0

            with open(temp_path, "wb") as output:
                while True:
                    msg_type, response_id, payload = recv_frame(sock)

                    if msg_type == DATA:
                        data_index, data_offset, data_length = (
                            struct.unpack_from("!IQI", payload, 0)
                        )
                        data = payload[16:]

                        if (
                            data_index != index
                            or data_offset != file_size
                            or data_length != len(data)
                        ):
                            raise RuntimeError("Invalid DATA frame")

                        output.write(data)
                        file_hash.update(data)
                        file_size += len(data)

                    elif msg_type == FILE_END:
                        end_index = struct.unpack("!I", payload)[0]
                        if end_index != index:
                            raise RuntimeError("FILE_END index mismatch")
                        break

                    else:
                        raise RuntimeError(
                            f"Unexpected message during transfer: {msg_type}"
                        )

            if file_size != expected_size:
                raise RuntimeError(
                    f"Size mismatch for {name}: "
                    f"{file_size} != {expected_size}"
                )

            if file_hash.digest() != expected_hash:
                raise RuntimeError(f"SHA-256 mismatch for {name}")

            os.replace(temp_path, final_path)

            # ACK contains file index and contiguous byte offset.
            ack_payload = struct.pack("!IQ", index, file_size)
            send_frame(sock, ACK, transfer_id, ack_payload)

            received_files += 1
            total_bytes += file_size
            print(f"  VERIFIED: {name}")

        msg_type, response_id, payload = recv_frame(sock)

        if msg_type != COMPLETE:
            raise RuntimeError(f"Expected COMPLETE, got {msg_type}")

        completed_count, completed_bytes = struct.unpack("!IQ", payload)

        if completed_count != received_files or completed_bytes != total_bytes:
            raise RuntimeError("COMPLETE totals do not match received files")

        print("\nSUCCESS")
        print(f"Verified files: {received_files}")
        print(f"Verified bytes: {total_bytes}")
        print(f"Output directory: {output_dir}")


if __name__ == "__main__":
    main()
