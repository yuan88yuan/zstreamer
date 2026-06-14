#!/usr/bin/env python3
import socket, time, sys

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', 8554))
s.settimeout(2)

def recv_response():
    data = b''
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
            if b'\r\n\r\n' in data:
                headers, rest = data.split(b'\r\n\r\n', 1)
                cl = None
                for line in headers.split(b'\r\n'):
                    if line.lower().startswith(b'content-length:'):
                        cl = int(line.split(b':')[1].strip())
                        break
                if cl is not None:
                    needed = len(headers) + 4 + cl
                    if len(data) >= needed:
                        break
                else:
                    break
        except socket.timeout:
            break
    return data

# 1. DESCRIBE
req = b'DESCRIBE rtsp://127.0.0.1:8554/colorbar RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n'
s.send(req)
resp = recv_response()
print('=== DESCRIBE ===')
text = resp.decode('utf-8', errors='replace')
print(text)

# 2. SETUP video (interleaved 0-1)
req = b'SETUP rtsp://127.0.0.1:8554/colorbar/trackID=0 RTSP/1.0\r\nCSeq: 2\r\nTransport: RTP/AVP/TCP;interleaved=0-1\r\n\r\n'
s.send(req)
resp = recv_response()
print('=== SETUP VIDEO ===')
text = resp.decode('utf-8', errors='replace')
print(text)

session_id = None
for line in text.split('\r\n'):
    if line.lower().startswith('session:'):
        session_id = line.split(':')[1].strip()
        break
print(f'Session ID: {session_id}')

# 3. SETUP audio (interleaved 2-3)
req = f'SETUP rtsp://127.0.0.1:8554/colorbar/trackID=1 RTSP/1.0\r\nCSeq: 3\r\nSession: {session_id}\r\nTransport: RTP/AVP/TCP;interleaved=2-3\r\n\r\n'.encode()
s.send(req)
resp = recv_response()
print('=== SETUP AUDIO ===')
print(resp.decode('utf-8', errors='replace'))

# 4. PLAY
req = f'PLAY rtsp://127.0.0.1:8554/colorbar RTSP/1.0\r\nCSeq: 4\r\nSession: {session_id}\r\nRange: npt=0.000-\r\n\r\n'.encode()
s.send(req)
resp = recv_response()
print('=== PLAY ===')
print(resp.decode('utf-8', errors='replace'))

# 5. Read interleaved RTP data (first 30 packets)
print('=== RTP DATA (first 30 packets) ===')
for i in range(30):
    try:
        header = s.recv(4)
        if not header or len(header) < 4:
            break
        if header[0] != ord('$'):
            print(f'  Unexpected: {header.hex()}')
            break
        channel = header[1]
        length = (header[2] << 8) | header[3]
        payload = s.recv(length)
        
        version = (payload[0] >> 6) & 3
        pt = payload[1] & 0x7f
        marker = (payload[1] >> 7) & 1
        seq = (payload[2] << 8) | payload[3]
        ts = (payload[4] << 24) | (payload[5] << 16) | (payload[6] << 8) | payload[7]
        ssrc = (payload[8] << 24) | (payload[9] << 16) | (payload[10] << 8) | payload[11]
        
        nal_info = ''
        if pt == 96 and len(payload) > 12:
            nal_type = payload[12] & 0x1f
            nal_names = {7: 'SPS', 8: 'PPS', 5: 'IDR', 1: 'SLICE', 6: 'SEI', 9: 'AUD'}
            nal_info = f' NAL={nal_names.get(nal_type, str(nal_type))}'
        
        print(f'  #{i:2d} ch={channel} len={length:4d} seq={seq:5d} ts={ts:10d} M={marker} PT={pt}{nal_info}')
    except socket.timeout:
        print('  (timeout)')
        break

s.close()
