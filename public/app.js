const $ = id => document.getElementById(id);
let version = 3;
async function api(path, options = {}) {
  const response = await fetch(path, { ...options, headers: {'Content-Type':'application/json', ...(options.headers || {})} });
  if (!response.ok) { let msg = `HTTP ${response.status}`; try {msg=(await response.json()).error || msg;} catch {} throw new Error(msg); }
  return response.status === 204 ? null : response.json();
}
function showError(err) { $('error').textContent = err?.message || String(err); }
function taskNode(task) {
  const el=document.createElement('div'); el.className='task';
  const check=document.createElement('input'); check.type='checkbox';check.checked=!!task.completed;
  check.setAttribute('aria-label','Đánh dấu hoàn thành');
  check.addEventListener('change',async()=>{try{await api(`/api/tasks/${task.id}`,{method:'PATCH',body:JSON.stringify({completed:check.checked})});await refresh();}catch(e){showError(e);check.checked=!check.checked;}});
  const name=document.createElement('span');name.className='name';name.textContent=task.title;
  const del=document.createElement('button');del.className='delete';del.textContent='Xóa';
  del.addEventListener('click',async()=>{try{await api(`/api/tasks/${task.id}`,{method:'DELETE'});await refresh();}catch(e){showError(e);}});
  el.append(check,name,del);return el;
}
async function refresh() {
  try {const tasks=await api('/api/tasks');$('error').textContent='';
    const todo=$('todo'),done=$('done');todo.replaceChildren();done.replaceChildren();
    let n=0;for(const task of tasks){(task.completed?done:todo).appendChild(taskNode(task));if(!task.completed)n++;}
    $('todo-count').textContent=n;$('done-count').textContent=tasks.length-n;$('count').textContent=`${tasks.length} công việc`;
    if(!n){const p=document.createElement('p');p.className='empty';p.textContent='Không có công việc';todo.appendChild(p);}
    if(version!==1&&!done.children.length){const p=document.createElement('p');p.className='empty';p.textContent='Chưa có công việc hoàn thành';done.appendChild(p);}
    if(version===1||version===2){todo.replaceChildren();for(const task of tasks)todo.appendChild(taskNode(task));}
  } catch(e){showError(e);}
}
$('new-task').addEventListener('submit',async e=>{e.preventDefault();const title=$('title').value.trim();if(!title)return;try{await api('/api/tasks',{method:'POST',body:JSON.stringify({title})});$('title').value='';await refresh();}catch(err){showError(err);}});
(async()=>{try{version=(await api('/api/version')).version;$('version').textContent=version===1?'V1 · Một client · Lưu JSON':version===2?'V2 · Client–Server · JSON dùng chung':'V3 · Kanban · SQLite';if(version<3)$('board').classList.add('list-mode');}catch(e){showError(e);}await refresh();if(version>=2)setInterval(refresh,2500);})();
