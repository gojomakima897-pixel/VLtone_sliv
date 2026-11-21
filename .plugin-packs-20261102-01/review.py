"""Exercise private draft transfer over the real HTTPS ingress before publication."""
import hashlib,http.client,json,os,secrets,subprocess,uuid
from pathlib import Path
from urllib.parse import urlparse,unquote,parse_qs
stage=Path('/opt/vlt-account-platform/.plugin-packs-20261102-01')
def sql(statement):
    db=urlparse(os.environ['DATABASE_URL']);options=parse_qs(db.query)
    env={**os.environ,'PGDATABASE':unquote(db.path.lstrip('/')),'PGHOST':db.hostname,'PGPORT':str(db.port or 5432),'PGUSER':unquote(db.username or ''),'PGPASSWORD':unquote(db.password or ''),'PGSSLMODE':options.get('sslmode',['prefer'])[0]}
    r=subprocess.run(['psql','-XAt','-v','ON_ERROR_STOP=1'],input=statement,text=True,capture_output=True,env=env)
    if r.returncode:raise RuntimeError('Database operation failed')
    return r.stdout.strip()
owner=str(uuid.UUID(sql("SELECT id FROM admin_users WHERE status='active' AND is_owner ORDER BY created_at LIMIT 1;")))
session=str(uuid.uuid4());token=secrets.token_urlsafe(48);csrf=secrets.token_hex(32)
sql(f"INSERT INTO admin_sessions (id,admin_user_id,token_hash,csrf_token,last_seen_at,expires_at,created_at) VALUES ('{session}','{owner}','{hashlib.sha256(token.encode()).hexdigest()}','{csrf}',now(),now()+interval '15 minutes',now());")
headers={'Cookie':'vlt_admin_session='+token,'X-CSRF-Token':csrf,'Origin':os.environ['ADMIN_ORIGIN']}
def connection():return http.client.HTTPSConnection('admin.vltstudio.ru',timeout=180)
try:
    c=connection();c.request('GET','/api/v1/admin/plugin-packs',headers=headers);r=c.getresponse();assert r.status==200;items=json.load(r)['packs'];c.close()
    item=next(p for p in items if p['id']=='vlt.cl1' and p['version']=='1.0.0')
    assert item['published_at'] is None, 'Review requires an unpublished draft'
    path='/api/v1/admin/plugin-packs/'+item['storage_id']+'/artifact'
    c=connection();c.request('GET',path);r=c.getresponse();assert r.status==401;r.read();c.close();print('PASS draft download requires administrator access')
    destination=stage/'server-roundtrip.vltpack';total=item['download_bytes'];digest=hashlib.sha256()
    with destination.open('wb') as out:
        for start,end in [(0,1048575),(1048576,total-1)]:
            c=connection();c.request('GET',path,headers={**headers,'Range':f'bytes={start}-{end}','If-Range':'"'+item['sha256']+'"'})
            r=c.getresponse();assert r.status==206,(r.status,r.read(300));assert r.getheader('ETag')=='"'+item['sha256']+'"';assert r.getheader('Content-Range')==f'bytes {start}-{end}/{total}'
            count=0
            while data:=r.read(1024*1024):out.write(data);digest.update(data);count+=len(data)
            assert count==end-start+1;c.close()
    assert digest.hexdigest()==item['sha256'];print('PASS HTTPS Range/ETag resume and complete archive SHA-256',total)
    c=connection();c.request('HEAD',path,headers=headers);r=c.getresponse();assert r.status==200 and int(r.getheader('Content-Length'))==total;r.read();c.close();print('PASS HEAD metadata')
    c=connection()
    with (stage/'vlt-cl1-1.0.0-windows-x64.vltpack').open('rb') as payload:
        c.request('POST','/release-upload/v1/admin/plugin-packs',body=payload,headers={**headers,'Content-Type':'application/octet-stream','Content-Length':str(total)});r=c.getresponse();result=json.load(r);assert r.status==409 and result['code']=='pack_exists',(r.status,result)
    c.close();print('PASS admin upload proxy validates archive and preserves immutable version')
finally:
    sql(f"DELETE FROM admin_sessions WHERE id='{session}';")
