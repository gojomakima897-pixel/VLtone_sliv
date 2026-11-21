"""Deployment validation through the normal admin API; no credentials persisted."""
import hashlib,http.client,json,os,secrets,subprocess,sys,uuid
from pathlib import Path
from urllib.parse import urlparse,unquote,parse_qs
stage=Path('/opt/vlt-account-platform/.plugin-packs-20261102-01')
archive=stage/'vlt-cl1-1.0.0-windows-x64.vltpack'
def sql(statement):
    db=urlparse(os.environ['DATABASE_URL']);options=parse_qs(db.query)
    env={**os.environ,'PGDATABASE':unquote(db.path.lstrip('/')),'PGHOST':db.hostname,'PGPORT':str(db.port or 5432),'PGUSER':unquote(db.username or ''),'PGPASSWORD':unquote(db.password or ''),'PGSSLMODE':options.get('sslmode',['prefer'])[0]}
    result=subprocess.run(['psql','-XAt','-v','ON_ERROR_STOP=1'],input=statement,text=True,capture_output=True,env=env)
    if result.returncode:raise RuntimeError('Database operation failed')
    return result.stdout.strip()
owner=str(uuid.UUID(sql("SELECT id FROM admin_users WHERE status='active' AND is_owner ORDER BY created_at LIMIT 1;")))
session=str(uuid.uuid4());token=secrets.token_urlsafe(48);csrf=secrets.token_hex(32)
sql(f"INSERT INTO admin_sessions (id,admin_user_id,token_hash,csrf_token,last_seen_at,expires_at,created_at) VALUES ('{session}','{owner}','{hashlib.sha256(token.encode()).hexdigest()}','{csrf}',now(),now()+interval '15 minutes',now());")
def request(method,path,body=None,content_type='application/json',size=None):
    c=http.client.HTTPConnection('127.0.0.1',8080,timeout=180)
    headers={'Cookie':'vlt_admin_session='+token,'X-CSRF-Token':csrf,'Origin':os.environ['ADMIN_ORIGIN'],'Content-Type':content_type}
    if size is not None:headers['Content-Length']=str(size)
    c.request(method,'/v1/admin/plugin-packs'+path,body=body,headers=headers);r=c.getresponse();payload=r.read();c.close()
    if r.status>=400:raise RuntimeError(f'Admin API returned {r.status}: '+payload.decode()[:500])
    return json.loads(payload)
try:
    items=request('GET','')['packs']
    item=next((p for p in items if p['id']=='vlt.cl1' and p['version']=='1.0.0' and p['platform']=='windows-x64'),None)
    digest=hashlib.file_digest(archive.open('rb'),'sha256').hexdigest()
    if item is None:
        with archive.open('rb') as f:item=request('POST','',f,'application/octet-stream',archive.stat().st_size)
    if item['sha256']!=digest:raise RuntimeError('Immutable version already exists with different bytes')
    if '--publish' in sys.argv:item=request('PUT','/'+item['storage_id']+'/publication',b'{"published":true}')
    if '--hide' in sys.argv:item=request('PUT','/'+item['storage_id']+'/publication',b'{"published":false}')
    (stage/'pack-publication.json').write_text(json.dumps(item,indent=2))
    print(json.dumps({'id':item['id'],'version':item['version'],'sha256':item['sha256'],'download_bytes':item['download_bytes'],'published_at':item.get('published_at'),'hidden':item['hidden']}))
finally:
    sql(f"DELETE FROM admin_sessions WHERE id='{session}';")
