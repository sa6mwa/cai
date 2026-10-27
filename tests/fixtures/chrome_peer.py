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
void_methods={'attach','detach','markTab','finalizeTabs','nameSession','turnEnded','executeCdp'}
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
    if method=='executeCdp' and request['params'].get('method')=='Runtime.evaluate':
        return {'result':{'type':'string','value':'Fixture page text'}}
    if method=='executeCdp' and request['params'].get('method')=='Page.captureScreenshot':
        return {'data': 'invalid' if scenario=='image-invalid' else None if scenario=='image-null' else PNG}
    return None

# Contract inspected in the official Chrome extension 1.26.1020.11845:
# its CDP bridge passes executeCdp.commandParams to debugger.sendCommand.
# JSON-RPC params is the envelope; it is not the nested command argument key.
def command_error(request):
    if request.get('method') != 'executeCdp': return None
    params=request.get('params', {})
    if not isinstance(params.get('commandParams'), dict) or 'params' in params:
        return {'code':1,'message':json.dumps({'code':-32602,
                'data':'Failed to deserialize params - CBOR: map start expected at position 0',
                'message':'Invalid parameters'})}
    return None

def record(request):
    with (root/'calls.jsonl').open('a') as output:
        output.write(json.dumps(request)+'\n')

if mode=='wire':
    sys.stdout.buffer.write(wire(json.loads(sys.argv[3])))
    sys.exit(0)
if mode=='extension':
    scenario=sys.argv[3] if len(sys.argv)>3 else 'success'
    void_scenario=scenario in ('void','manual-detach')
    attached=set()
    while True:
        try: request=receive(sys.stdin.buffer)
        except EOFError: break
        if 'method' not in request:
            with (root/'host-replies.jsonl').open('a') as output:
                output.write(json.dumps(request)+'\n')
            continue
        record(request)
        value={'jsonrpc':'2.0','id':request.get('id'),'result':result(request,'success')}
        if scenario=='stop-cleanup-fail' and request.get('method')=='turnEnded':
            value.pop('result'); value['error']={'code':-32000,'message':'Cleanup fixture rejected turnEnded'}
        failure=command_error(request)
        if failure is not None:
            value.pop('result'); value['error']=failure
        if request.get('method')=='attach':
            attached.add(request['params']['tabId'])
        if request.get('method')=='detach':
            attached.discard(request['params']['tabId'])
        if request.get('method')=='turnEnded':
            with (root/'active-tabs.jsonl').open('a') as output:
                output.write(json.dumps({'session_id':request['params']['session_id'],'tabs':sorted(attached)})+'\n')
        if void_scenario and 'error' not in value and request.get('method') in void_methods and value['result'] is None:
            value.pop('result')
        if scenario=='manual-detach' and request.get('method')=='detach':
            value['error']={'code':-32000,'message':'Debugger is not attached'}
            with (root/'cleanup-errors.jsonl').open('a') as output:
                output.write(json.dumps(value)+'\n')
        if void_scenario and request.get('method')=='attach':
            sys.stdout.buffer.write(wire({'jsonrpc':'2.0','id':'extension-ping','method':'ping','params':{}}))
            sys.stdout.buffer.write(wire({'jsonrpc':'2.0','method':'onCDPEvent','params':{'source':{'tabId':7},'method':'Page.loadEventFired','params':{}}}))
        sys.stdout.buffer.write(wire(value)); sys.stdout.buffer.flush()
        if scenario=='manual-detach' and request.get('method')=='attach':
            attached.discard(request['params']['tabId'])
            sys.stdout.buffer.write(wire({'jsonrpc':'2.0','method':'onCDPDetach','params':{'tabId':request['params']['tabId'],'reason':'canceled_by_user'}}))
            sys.stdout.buffer.flush()
        if void_scenario and request.get('method')=='turnEnded' and request.get('params',{}).get('session_id')=='abandoned':
            sys.stdout.buffer.write(wire({'jsonrpc':'2.0','id':'cleanup-ping','method':'ping','params':{}}))
            sys.stdout.buffer.flush()
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
                failure=command_error(request)
                if failure is not None and scenario=='success':
                    value.pop('result'); value['error']=failure
                if scenario=='identity-missing-result' and method=='getInfo':
                    value.pop('result')
                if scenario=='void-cdp-missing-image' and method=='executeCdp':
                    value['result']=None
                if scenario.startswith('void') and method in void_methods and value['result'] is None:
                    value.pop('result')
                    if method=='attach':
                        if scenario=='void-wrong-id': value['id']='wrong'
                        elif scenario=='void-no-id': value.pop('id')
                        elif scenario=='void-error': value['error']={'code':-32000,'message':'Debugger unattached'}
                        elif scenario=='void-malformed-error': value['error']={'code':-32000}
                        elif scenario=='void-both': value.update(result=None,error={'code':-32000,'message':'Debugger unattached'})
                        elif scenario=='void-version': value['jsonrpc']='1.0'
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
