#!/usr/bin/env python3
"""Chrome native-message data peers. Test assertions and commands live in Bash."""
import json
from pathlib import Path
import socket
import struct
import sys
import time

PNG='iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aAn8AAAAASUVORK5CYII='
identity={'type':'extension','family':'chrome','name':'Chrome'}
mode=sys.argv[1]
root=Path(sys.argv[2])

def wire(value):
    data=json.dumps(value).encode()
    return struct.pack('=I',len(data))+data

def receive(stream):
    def exact(size):
        data=bytearray()
        while len(data)<size:
            chunk=stream.recv(size-len(data)) if hasattr(stream,'recv') else stream.read(size-len(data))
            if not chunk: raise EOFError
            data.extend(chunk)
        return bytes(data)
    return json.loads(exact(struct.unpack('=I',exact(4))[0]))

def result(request, scenario):
    method=request.get('method')
    if method=='getInfo':
        return {'type':'iab','family':'chrome'} if scenario=='identity' else identity
    if method in ('createTab','claimUserTab'): return {'id':7,'title':'Fixture'}
    if method=='getUserTabs': return None if scenario=='null' else [{'id':7,'title':'Fixture'}]
    if method=='executeCdp' and request['params'].get('method')=='Page.captureScreenshot':
        return {'data': 'invalid' if scenario=='image-invalid' else None if scenario=='image-null' else PNG}
    return None

def record(request):
    with (root/'calls.jsonl').open('a') as output:
        output.write(json.dumps(request)+'\n')

if mode=='wire':
    sys.stdout.buffer.write(wire(json.loads(sys.argv[3])))
    sys.exit(0)
if mode=='extension':
    while True:
        try: request=receive(sys.stdin.buffer)
        except EOFError: break
        record(request)
        value={'jsonrpc':'2.0','id':request.get('id'),'result':result(request,'success')}
        sys.stdout.buffer.write(wire(value)); sys.stdout.buffer.flush()
else:
    scenario=sys.argv[3]
    path=root/'mock.sock'
    listener=socket.socket(socket.AF_UNIX)
    listener.bind(str(path)); path.chmod(0o600); listener.listen(4)
    (root/'ready').write_text('ready')
    connection,_=listener.accept()
    connection.settimeout(5)
    with connection:
        try:
            while True:
                request=receive(connection); record(request)
                method=request.get('method')
                value={'jsonrpc':'2.0','id':request.get('id'),'result':result(request,scenario)}
                if method!='getInfo':
                    if scenario=='failure': value={'jsonrpc':'2.0','id':request['id'],'error':{'code':-32000,'message':'Debugger unattached'}}
                    elif scenario=='wrong-id': value['id']='wrong'
                    elif scenario=='missing-result': value.pop('result')
                    elif scenario=='limit': value['result']='x'*300
                    elif scenario=='oversized': connection.sendall(struct.pack('=I',0xffffffff)); break
                    elif scenario=='malformed': connection.sendall(struct.pack('=I',2)+b'{{'); break
                    elif scenario=='truncated': connection.sendall(struct.pack('=I',10)+b'short'); break
                    elif scenario in ('timeout','cancel'): time.sleep(.7); break
                    if scenario=='fragment':
                        connection.sendall(wire({'jsonrpc':'2.0','method':'onCDPEvent','params':{}}))
                        for byte in wire(value): connection.sendall(bytes([byte]))
                        continue
                connection.sendall(wire(value))
        except (EOFError,BrokenPipeError,ConnectionResetError): pass
    listener.close()
