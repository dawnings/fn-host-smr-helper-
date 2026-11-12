'use strict';
const $=s=>document.querySelector(s), base=location.pathname.replace(/\/?$/,'/'), state={data:null,disk:null,busy:false};
const services=[['identify','自动识别','开机执行识别，已识别的硬盘直接跳过。主要用于解决硬盘经 ASM SATA 控制器后无法自动识别的问题。'],['mapping','自动映射与挂载','恢复已管理硬盘的映射，挂载外置空间；原生空间交由飞牛挂载。'],['association','飞牛硬盘归属','让飞牛把已初始化的原生 SMR 空间归属到对应存储空间，硬盘页不再显示“未使用”。开启时会重启飞牛管理服务 trim_main。']];
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const size=n=>Number(n)?(Number(n)/1099511627776).toFixed(2)+' TB':'—';
function notice(s,error=false){const n=$('#notice');if(!n)return;n.textContent=s;n.hidden=!s;n.classList.toggle('error',error);n.classList.remove('busy')}
function noticeBusy(s){const n=$('#notice');if(!n)return;n.textContent=s;n.hidden=false;n.classList.remove('error');n.classList.add('busy')}
function askConfirm(body,ok='继续'){return new Promise(res=>{state.ask=res;$('#ask-body').textContent=body;$('#ask-ok').textContent=ok;$('#ask').showModal()})}
const actionLabel=q=>({identify:'手动识别','identify-force':'强制尝试',initialize:'初始化',mount:'挂载',unmount:'卸载',activate:'恢复映射',release:'解除映射',service:'服务设置',policy:'控制器设置'})[q.action]||q.action
async function refresh(force=false){try{const r=await fetch(base+'api/state');if(!r.ok)throw Error(await r.text());const next=await r.json();if(force||JSON.stringify(next)!==JSON.stringify(state.data)){state.data=next;render();}}catch(e){notice(e.message,true)}}
// 每个区块单独渲染：某一块出错只在提示条报告，不让整页空白。
function render(){const d=state.data,problems=[];state.busy=d.job?.status==='running';
 for(const [name,fn] of [['版本',renderVersion],['依赖',renderDependencies],['服务',renderServices],['控制器',renderControllers],['硬盘',renderDisks],['操作',renderJob]]){try{fn(d)}catch(e){problems.push(`${name}区块显示失败：${e.message}`)}}
 const pending=(d.journal||[]).filter(j=>j.needs_attention).map(j=>j.summary);
 const messages=[...problems,...pending,...(d.warnings||[]),d.error,d.job?.status==='failed'?d.job.output:''].filter(Boolean);
 notice(messages.join('\n'),messages.length>0)}
function renderVersion(d){$('#version').textContent='0.1.14 测试版'+(d.version?' · SATA 后端 '+d.version:'')}
function renderDependencies(d){const note=$('#deps-note'),text=$('#deps-text');if(!note||!text)return;const dep=d.dependencies;if(!dep){note.hidden=true;return}const missing=(dep.items||[]).filter(i=>!i.ok);if(!missing.length){note.hidden=true;return}const lines=missing.map(i=>{const cat=i.category==='hard'?'必须':'兼容';return `${cat}：${i.name}${i.suggestion?'（建议：'+i.suggestion+'）':''}`});text.textContent=(dep.compat_ok?'':'兼容识别不可用；')+'依赖缺失：'+lines.join('；');note.hidden=false;note.classList.toggle('error',!dep.hard_ok)}
function renderServices(d){const on=d.services||d.config?.services||{};const list=services.filter(([id])=>id!=='association'||d.association_supported!==false);$('#service-list').innerHTML=list.map(([id,title,desc])=>`<div class="row"><div class="row-info"><div class="row-title">${title}</div><div class="row-desc">${desc}</div></div><span class="caption">${on[id]?'已注册':'未注册'}</span><button class="switch" role="switch" aria-label="${title}" aria-checked="${!!on[id]}" data-service="${id}" ${state.busy?'disabled':''}></button></div>`).join('');
 $('#service-list').querySelectorAll('[data-service]').forEach(b=>b.onclick=()=>{
  const feature=b.dataset.service, enabled=b.getAttribute('aria-checked')!=='true';
  if(feature==='association'&&enabled)enableAssociation();
  else act({action:'service',feature,enabled});
 })}
async function enableAssociation(){
 if(state.busy||state.ask)return;
 if(!await askConfirm('开启飞牛硬盘归属会重启 trim_main，所有正在进行的文件操作都会中断，未完成的操作可能丢失。请先等文件操作结束，再开启。','开启并重启'))return;
 if(state.busy)return;
 state.busy=true;
 noticeBusy('正在开启飞牛硬盘归属…');
 try{
  const response=await fetch(base+'api/association-enable',{
   method:'POST',headers:{'X-CSRF-Token':state.data.csrf},body:''
  });
  const data=await response.json().catch(()=>({}));
  if(!response.ok||!data.restart_queued)throw Error(data.error||`开启失败（HTTP ${response.status}）`);
  notice('飞牛硬盘归属已开启，trim_main 即将重启。连接断开后请重新登录。');
 }catch(error){notice(error.message,true)}
 finally{state.busy=false}
}
function renderControllers(d){const controllers=d.controllers||[];$('#controller-list').innerHTML=controllers.map(c=>{const ports=(c.ports||[]).map(p=>`${esc(p.name)}：${p.disks?.length?esc(p.disks.join('、')):'空'}`).join(' · ');const policy=c.policy||{};const supported=c.policy_supported!==false;const id=esc(c.identity);const options=supported?`<div class="controller-options"><label>强制关闭 NCQ<button class="switch" role="switch" aria-label="强制关闭 NCQ" aria-checked="${!!policy.disable_ncq}" data-policy-q="${id}" ${state.busy?'disabled':''}></button></label><label>强制 32 位 DMA<button class="switch" role="switch" aria-label="强制 32 位 DMA" aria-checked="${!!policy.dma32}" data-policy-m="${id}" ${state.busy?'disabled':''}></button></label></div>`:'<span class="caption">此控制器不支持独立策略</span>';return `<div class="controller-row"><div class="row-info"><div class="row-title">${esc(c.driver||'未知驱动')} · ${esc(c.address)}</div><div class="row-desc">${ports||'无端口'}${c.note?' · '+esc(c.note):''} · 控制器设置重启后生效</div></div>${options}</div>`}).join('')||'<div class="empty">未发现 SATA 控制器</div>';
$('#controller-list').querySelectorAll('[data-policy-q],[data-policy-m]').forEach(b=>b.onclick=()=>{const id=b.dataset.policyQ??b.dataset.policyM;const c=controllers.find(x=>x.identity===id);const p=c?.policy||{};const isQ=b.dataset.policyQ!==undefined;act({action:'policy',controller:id,disable_ncq:isQ?!p.disable_ncq:!!p.disable_ncq,dma32:isQ?!!p.dma32:!p.dma32})})}
let zoneStatus = {};
let reclaiming = false;

async function refreshZoned() {
 try {
  const {disks}=await portRequest({cache:'no-store'},'zoned-status');
  zoneStatus=disks;
 }catch(error){
  zoneStatus={};
  notice('无法读取区带状态：'+error.message,true);
 }
 if(state.data)renderDisks(state.data);
}

async function reclaimDisk(serial) {
 if(reclaiming||state.busy||!zoneStatus[serial]||!state.data?.csrf)return;
 const mapping=zoneStatus[serial].name;
 if(!await askConfirm(`对 ${serial}（${mapping}）启动区带回收？回收可能增加磁盘 I/O，不删除文件。`,'清理'))return;
 reclaiming=true;
 renderDisks(state.data);
 try{
  const result=await portRequest({method:'POST',headers:{'Content-Type':'application/json',
   'X-CSRF-Token':state.data.csrf},body:JSON.stringify({serial})},'zoned-status');
  await refreshZoned();
  notice(`${result.mapping} 已提交后台回收。请稍后刷新查看区带计数。`);
 }catch(error){notice(error.message,true)}
 finally{reclaiming=false;renderDisks(state.data)}
}

function diskStorage(disk, records){
 const record=(records||[]).find(item=>item.serial===disk.serial);
 const mode=disk.managed?.mode||record?.mode||
  (['native','external'].includes(disk.format)?disk.format:'');
 const mounts=Array.isArray(disk.mounts)?disk.mounts.filter(Boolean):[];
 const type={native:'飞牛原生',external:'外置盘'}[mode];
 const mountStatus=mounts.length?'已挂载':'未挂载';
 return {mode,mounts,mountStatus,label:type?`${type} · ${mountStatus}`:(disk.busy?'已使用':'未管理')};
}
function renderDisks(d){const disks=d.disks||[], managed=d.config?.disks||[];$('#disk-list').innerHTML=disks.length?disks.map(x=>{const storage=diskStorage(x,managed);const zoned=zoneStatus[x.serial];return `<div class="row"><div class="disk-icon">▤</div><div class="row-info"><div class="row-title">${esc(x.model||x.name)}</div><div class="row-desc">${esc(x.serial)} · ${size(x.size)} · ${esc(x.name)}</div>${zoned?`<div class="row-desc">${esc(zoned.name)} · ${esc(zoned.zones)} 个区带 · 随机 ${esc(zoned.random)} · 顺序 ${esc(zoned.sequential)}${zoned.cache?` · 缓存 ${esc(zoned.cache)}`:''}（空闲 / 总数）</div>`:''}</div><span class="status ${storage.mode?'':'dim'}">${esc(storage.label)}</span>${zoned?`<button data-reclaim="${esc(x.serial)}" ${reclaiming||state.busy?'disabled':''}>清理</button>`:''}<button data-manage="${esc(x.serial)}">管理</button></div>`}).join(''):'<div class="empty">未发现主机管理型 SMR 硬盘</div>';
 $('#disk-list').querySelectorAll('[data-manage]').forEach(b=>b.onclick=()=>manage(disks.find(x=>x.serial===b.dataset.manage)));
 $('#disk-list').querySelectorAll('[data-reclaim]').forEach(b=>b.onclick=()=>reclaimDisk(b.dataset.reclaim));
 $('#managed-count').textContent=managed.length+' 块';$('#managed-list').innerHTML=managed.map(x=>`<div class="row"><div class="disk-icon">▤</div><div class="row-info"><div class="row-title">${esc(x.serial)}</div><div class="row-desc">${x.mode==='native'?'飞牛原生存储':'外置挂载'} · ${disks.some(y=>y.serial===x.serial)?diskStorage(disks.find(y=>y.serial===x.serial),managed).mountStatus:'未连接'}</div></div><span class="caption">${esc(disks.find(y=>y.serial===x.serial)?.mounts?.join(', ')||'')}</span></div>`).join('')||'<div class="empty">尚未添加硬盘，在识别页选择硬盘进行管理</div>';
 $('#identify').disabled=state.busy}
function renderJob(d){if(!d.job)return;$('#job-state').textContent=({running:'执行中',done:'完成',failed:'失败'})[d.job.status]||'';$('#job-output').textContent=d.job.output||'正在执行…'}
async function act(q){if(state.busy)return;state.busy=true;notice('');const label=actionLabel(q);noticeBusy(`${label}：正在执行…（构建/更新 initramfs 时可能需要 1-2 分钟，请勿关闭页面）`);try{const r=await fetch(base+'api/action',{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':state.data.csrf},body:JSON.stringify(q)});if(!r.ok)throw Error(await r.text());await r.json();$('#activity').open=true;const started=Date.now();let job=null;while(Date.now()-started<300000){await new Promise(res=>setTimeout(res,1200));let st=null;try{st=await (await fetch(base+'api/state')).json()}catch(e){}if(st){state.data=st;const j=st.job;if(j&&j.status!=='running'){job=j;break}}noticeBusy(`${label}：执行中… ${Math.round((Date.now()-started)/1000)} 秒`)}if(!job)throw Error(`${label}超时，请刷新页面查看结果`);state.busy=false;await refresh(true);$('#notice').classList.remove('busy');if(job.status==='failed')notice(job.output||`${label}失败`,true);else notice(`${label}完成${job.output?'：'+job.output:''}`)}catch(e){state.busy=false;$('#notice').classList.remove('busy');notice(e.message,true);try{render()}catch(_){}}}
function canRequestFormat(disk, mode, mounted, appBusy){
 if(mounted||appBusy)return false;
 if(mode==='native'){
  // The original backend rechecks fnOS ownership before any destructive action.
  return disk.managed?.mode==='native'&&disk.mapping_active===false&&disk.vg_active!==true;
 }
 return !disk.busy;
}
function manage(d){
 state.disk=d;
 $('#disk-title').textContent='硬盘管理';
 const m=d.managed||null;
 const {mode,mounts}=diskStorage(d,state.data?.config?.disks);
 const mounted=mounts.length>0;
 const mapped=d.mapping_active===true;
 const vgActive=d.vg_active===true;
 const formats={native:'飞牛原生空间（由飞牛挂载）',external:'外置盘（由本应用挂载）',unmapped:'尚未格式化',other:'其他或未知格式'};
 let status;
 if(mode==='native') status=mounted?`已在飞牛中挂载：${mounts.join(', ')}`:
  (mapped?'未挂载，请到飞牛存储空间管理挂载':'未挂载，映射未激活');
 else if(mode==='external') status=mounted?'已挂载，可直接使用':(mapped?'已就绪，未挂载':'未激活');
 else status=d.busy?'已使用':(d.format==='unmapped'?'尚未格式化':'格式或用途未知');

 const kv=[['型号',d.model],['序列号',d.serial],['容量',size(d.size)],['当前格式',formats[mode||d.format]||formats.other],['当前状态',status]];
 $('#disk-details').innerHTML=kv.map(([k,v])=>`<div class="kv"><span>${k}</span><span>${esc(v)}</span></div>`).join('');

 const actions=[];
 if(m){
  if(mode==='external'&&mounted) actions.push({act:'unmount',label:'卸载',primary:true});
  if(mode==='external'&&!mounted&&mapped) actions.push({act:'mount',label:'挂载使用'});
  if(!mapped||(mode==='native'&&!vgActive)) actions.push({act:'activate',label:'恢复映射'});
  if(mapped) actions.push({act:'release',label:'解除映射',disabled:mounted,reason:mounted?(mode==='native'?'请先在飞牛中卸载':'请先卸载'):''});
 }
 const formatDisabled=!canRequestFormat(d,mode,mounted,state.busy);
 $('#disk-actions').innerHTML=actions.map(a=>`<button data-act="${a.act}" class="${a.primary?'primary':''}" ${(state.busy||a.disabled)?'disabled':''} title="${esc(a.reason||'')}">${a.label}</button>`).join('')+
  `<button data-format="external" ${formatDisabled?'disabled':''}>格式化为外置盘</button><button data-format="native" ${formatDisabled?'disabled':''}>格式化为飞牛原生</button>`;

 const hint=$('#disk-busy-hint');
 if(hint){
  let text='';
  if(mode==='native'){
   if(mounted) text='已在飞牛中挂载。如需重新格式化，请先到飞牛「存储空间管理」删除对应空间，再解除映射。';
   else if(mapped||vgActive) text='飞牛原生空间仍有活动映射。若要重新格式化，请先在飞牛删除对应空间，再解除映射。';
   else if(formatDisabled) text='该盘有飞牛原生记录，但应用无法确认已安全解除映射。请先核对存储空间和登记状态。';
   else text='映射已解除。若要保留数据，请选择「恢复映射」。若要重新格式化，请先确认飞牛「存储空间管理」中已删除对应空间，后端会再次检查。';
  }
  else if(m&&mounted) text='已挂载，可以直接使用。若要格式化，请先点“卸载”；格式化会清空整块盘并自动解除映射。';
  else if(m&&mapped) text='映射已激活但未挂载。点“挂载使用”即可访问；也可以直接格式化，但会清空整块盘。';
  else if(m) text='映射未激活。点“恢复映射”后可继续使用；格式化会清空整块盘。';
  else if(d.busy||d.format!=='unmapped') text='磁盘已使用或格式尚未确认。如有现存飞牛空间，请先在服务页检查「重建登记」。';
  else text='尚未格式化。选“格式化为外置盘”由本应用挂载到 /vol00；选“格式化为飞牛原生”后到飞牛「存储空间管理」挂载并处理权限。';
  hint.textContent=text;
  hint.hidden=false;
 }

 $('#disk-actions').querySelectorAll('[data-act]').forEach(b=>b.onclick=async()=>{
  if(b.dataset.act==='release'&&!(await askConfirm('解除映射后，盘上的数据还在，但需要重新识别/恢复映射才能访问。确定解除？','解除映射')))return;
  $('#manage').close();
  act({action:b.dataset.act,serial:d.serial});
 });
 $('#disk-actions').querySelectorAll('[data-format]').forEach(b=>b.onclick=()=>{
  $('#manage').close();
  $('#init-target').textContent=d.model+' · '+d.serial+' · '+size(d.size);
  $('#init-mode').value=b.dataset.format;
  $('#init-mode').onchange();
  $('#init-confirm').value='';
  $('#init-submit').disabled=true;
  $('#initialize').showModal();
  setTimeout(()=>$('#init-confirm').focus(),50);
 });
 $('#manage').showModal();
}
document.querySelectorAll('[data-page]').forEach(b=>b.onclick=()=>{document.querySelectorAll('[data-page]').forEach(x=>x.classList.toggle('selected',x===b));$('#services').hidden=b.dataset.page!=='services';$('#disks').hidden=b.dataset.page!=='disks';refresh()});
async function portRequest(options, endpoint='port-identify') {
 const response=await fetch(base+'api/'+endpoint,options);
 if(!response.ok){
  const data=await response.json().catch(()=>({}));
  throw Error(data.error||`请求失败（HTTP ${response.status}）`);
 }
 return response.json();
}

$('#rebuild-registry').onclick=async()=>{
 const button=$('#rebuild-registry');
 button.disabled=true;
 try{
  const {records}=await portRequest({cache:'no-store'},'native-registry');
  if(!records.length){notice('没有发现可安全补回的飞牛原生登记。');return}
  const names=records.map(d=>d.serial+' · '+d.dm).join('、');
  if(!await askConfirm('将补回以下原生存储登记：'+names+'。此操作只修改应用配置。','重建登记'))return;
  const result=await portRequest({method:'POST',headers:{'Content-Type':'application/json',
   'X-CSRF-Token':state.data.csrf},body:JSON.stringify({records})},'native-registry');
  await refresh(true);
  notice(`已重建 ${result.records.length} 条飞牛原生登记。`);
 }catch(error){notice(error.message,true)}
 finally{button.disabled=false}
};

async function openPortIdentify(){
 const button=$('#identify-force');
 const dialog=$('#port-identify');
 const select=$('#port-select');
 const submit=$('#port-submit');
 button.disabled=true;
 try{
  const data=await portRequest({cache:'no-store'});
  select.replaceChildren(new Option('请选择端口',''));
  for(const port of data.ports){
   const option=new Option(`${port.controller} · ${port.ata}${port.occupied?'（已有设备）':''}`,port.id);
   option.disabled=port.occupied;
   select.append(option);
  }
  submit.disabled=true;
  dialog.showModal();
 }catch(error){notice(error.message,true)}
 finally{button.disabled=false}
}

$('#port-select').onchange=()=>{$('#port-submit').disabled=!$('#port-select').value};
$('#port-identify-form').onsubmit=async event=>{
 event.preventDefault();
 const port=$('#port-select').value;
 if(!port||!state.data?.csrf)return;
 const button=$('#identify-force');
 $('#port-identify').close();
 button.disabled=true;
 noticeBusy(`正在对 ${port} 执行一次 Host-SMR 兼容识别…`);
 try{
  const data=await portRequest({method:'POST',headers:{'Content-Type':'application/json',
   'X-CSRF-Token':state.data.csrf},body:JSON.stringify({port})});
  await refresh(true);
  notice(data.disks.length?`已识别 Host-SMR：${data.disks.map(d=>'/dev/'+d).join('、')}`:
   (data.evidence?'检测到 ZAC 签名，硬盘仍未通过识别。':'未检测到本次复位产生的 ZAC 签名。'),!data.disks.length);
 }catch(error){notice(error.message,true)}
 finally{button.disabled=false}
};
$('#port-cancel').onclick=()=>$('#port-identify').close();

$('#refresh').onclick=async()=>{await refresh(true);await refreshZoned()};$('#identify').onclick=()=>act({action:'identify'});$('#identify-force').onclick=openPortIdentify;$('#about').onclick=()=>$('#help').showModal();$('#init-cancel').onclick=()=>$('#initialize').close();$('#init-mode').onchange=()=>{$('#init-submit').textContent=$('#init-mode').value==='native'?'清空并格式化为飞牛原生':'清空并格式化为外置盘'};$('#init-confirm').oninput=()=>$('#init-submit').disabled=$('#init-confirm').value!==state.disk?.serial;
$('#init-form').onsubmit=e=>{e.preventDefault();if($('#init-confirm').value!==state.disk.serial)return;$('#initialize').close();act({action:'initialize',serial:state.disk.serial,mode:$('#init-mode').value,confirm:$('#init-confirm').value})};
refresh().then(refreshZoned);setInterval(refresh,4000);

document.querySelectorAll('[data-ask-cancel]').forEach(b=>b.onclick=()=>{if(state.ask){state.ask(false);state.ask=null}$('#ask').close()});
$('#ask-ok').onclick=()=>{if(state.ask){state.ask(true);state.ask=null}$('#ask').close()};
$('#ask').addEventListener('cancel',e=>{e.preventDefault();if(state.ask){state.ask(false);state.ask=null}$('#ask').close()});

'use strict';

(() => {
  const list = document.querySelector('#usb-scan-list');
  const summary = document.querySelector('#usb-scan-summary');
  const button = document.querySelector('#usb-scan-refresh');
  const base = location.pathname.replace(/\/?$/, '/');
  const statusLabels = {
    'host-managed': '已识别为主机管理型 SMR',
    'block-device-present': '已生成磁盘设备',
    'storage-driver-without-block-device': '存储驱动已加载，尚无磁盘设备',
    'usb-device-present': 'USB 设备已枚举',
  };

  function addRow(title, description) {
    const row = document.createElement('div');
    const info = document.createElement('div');
    const name = document.createElement('div');
    const detail = document.createElement('div');
    row.className = 'row';
    info.className = 'row-info';
    name.className = 'row-title';
    detail.className = 'row-desc';
    name.textContent = title;
    detail.textContent = description;
    info.append(name, detail);
    row.append(info);
    list.append(row);
  }

  async function refreshUSB() {
    if (button.disabled) return;
    button.disabled = true;
    summary.textContent = '扫描中…';
    try {
      const response = await fetch(base + 'api/usb-scan', {cache: 'no-store'});
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
      list.replaceChildren();
      summary.textContent = `${data.usb_controllers.length} 个 USB 主控，${data.usb_devices.length} 个外接 USB 设备`;
      for (const controller of data.usb_controllers) {
        const hubs = controller.root_hubs.map(hub =>
          hub.speed_mbps ? `${hub.name} (${hub.speed_mbps} Mb/s)` : hub.name);
        addRow('USB 主控 ' + controller.path.split('/').pop(), hubs.join(' · '));
      }
      for (const device of data.usb_devices) {
        const disks = device.disks.map(disk =>
          `/dev/${disk.name}${disk.model ? ' ' + disk.model : ''} (${disk.zoned || '分区式能力未知'})`);
        addRow(`${device.product || 'USB 设备'} · ${device.vid_pid}`, [
          device.port,
          statusLabels[device.status] || device.status,
          ...device.drivers,
          ...disks,
        ].join(' · '));
      }
      if (!data.usb_devices.length) {
        addRow('未发现外接 USB 设备', '插上硬盘盒后点击「扫描 USB」。');
      }
    } catch (error) {
      list.replaceChildren();
      summary.textContent = 'USB 扫描失败';
      addRow('无法读取 USB 状态', error.message);
    } finally {
      button.disabled = false;
    }
  }

  button.addEventListener('click', refreshUSB);
  document.querySelector('#refresh').addEventListener('click', refreshUSB);
  refreshUSB();
})();
