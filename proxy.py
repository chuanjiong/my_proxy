

import sys
import time
import socket
import _thread
import winreg


PROXY_IP = '192.168.102.166'
PROXY_PORT = 22401
PROXY_SUB_NET = ('192.168.0.0', '255.255.0.0')

PAC_PORT = PROXY_PORT

BUF_SIZE = 2*1024*1024

pac_content = ''


def recv_request(sock):
    buf = b''
    header_size = 0
    content_length = 0
    while True:
        buf += sock.recv(BUF_SIZE)
        header_size = buf.find(b'\r\n\r\n')
        if header_size >= 0:
            header_size += 4
            break
    headers = buf[:header_size].split(b'\r\n')
    for header in headers:
        if header.find(b'Content-Length:') >= 0:
            content_length = int(header[header.find(b':')+1:])
            break
    while content_length+header_size > len(buf):
        buf += sock.recv(BUF_SIZE)
    return buf


def gen_pac_and_set(proxy_ip, proxy_port, proxy_sub_net):
    global pac_content
    pac_content = f'''
    function FindProxyForURL(url, host) {{
        if (isPlainHostName(host) == true)
            return "DIRECT";
        if (isInNet(host, "{proxy_sub_net[0]}", "{proxy_sub_net[1]}") == true)
            return "PROXY {proxy_ip}:{proxy_port}";
        return "DIRECT";
    }}
    '''
    print(pac_content)
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Internet Settings', 0, winreg.KEY_ALL_ACCESS) as key:
        url = f'http://127.0.0.1:{PAC_PORT}'
        print(f'set value: AutoConfigURL {url}')
        winreg.SetValueEx(key, 'AutoConfigURL', 0, winreg.REG_SZ, url)


gen_pac_and_set(PROXY_IP, PROXY_PORT, PROXY_SUB_NET)


def pac_client(client_connection, client_address):
    global pac_content
    req = recv_request(client_connection)
    print('pac recv', req)
    client_connection.send(b'HTTP/1.1 200 OK\r\n')
    client_connection.send(b'Server: my_proxy\r\n')
    client_connection.send(b'Content-Type: application/x-ns-proxy-autoconfig\r\n')
    client_connection.send(bytes(f'Content-Length: {len(pac_content)}\r\n', encoding='utf-8'))
    client_connection.send(b'Connection: Close\r\n\r\n')
    client_connection.send(bytes(pac_content, encoding='utf-8'))
    client_connection.close()

def pac_server(port):
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind(('0.0.0.0', port))
    server_socket.listen(1024)
    print(f'pac listen {port} ...')
    while True:
        client_connection, client_address = server_socket.accept()
        print(f'pac accept {client_address}')
        _thread.start_new_thread(pac_client, (client_connection, client_address))


_thread.start_new_thread(pac_server, (PAC_PORT,))


try:
    while True:
        time.sleep(10)
except:
    pass
finally:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Internet Settings', 0, winreg.KEY_ALL_ACCESS) as key:
        print(f'delete value: AutoConfigURL')
        winreg.DeleteValue(key, 'AutoConfigURL')

print(f'proxy end')

