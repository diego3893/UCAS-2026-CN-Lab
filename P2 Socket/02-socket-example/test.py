import requests
from os.path import dirname, realpath

requests.packages.urllib3.disable_warnings()

def read_html():
    return open(test_dir + '/index.html', 'rb').read()

test_dir = dirname(realpath(__file__))

server_ip = "10.0.0.1"

# timeout: 2 seconds
# "'proxies': {}", to ingore system proxy setting
http_params = { 'allow_redirects': False, 'timeout': 2, 'proxies': {} }
https_params = { 'verify': False, 'timeout': 2 , 'proxies': {} }

# http 301
r = requests.get(f'http://{server_ip}/index.html', **http_params)
assert(r.status_code == 301 and r.headers['Location'] == f'https://{server_ip}/index.html')

# https 200 OK
r = requests.get(f'https://{server_ip}/index.html', **https_params)
assert(r.status_code == 200 and read_html() == r.content)

# http 200 OK
r = requests.get(f'http://{server_ip}/index.html', **https_params)
assert(r.status_code == 200 and read_html() == r.content)

# http 404
r = requests.get(f'http://{server_ip}/notfound.html', **https_params)
assert(r.status_code == 404)

# file in directory
r = requests.get(f'http://{server_ip}/dir/index.html', **https_params)
assert(r.status_code == 200 and read_html() == r.content)

# http 206
headers = { 'Range': 'bytes=100-200' }
r = requests.get(f'http://{server_ip}/index.html', headers=headers, **https_params)
assert(r.status_code == 206 and read_html()[100:201] == r.content)

# http 206
headers = { 'Range': 'bytes=100-' }
r = requests.get(f'http://{server_ip}/index.html', headers=headers, **https_params)
assert(r.status_code == 206 and read_html()[100:] == r.content)
