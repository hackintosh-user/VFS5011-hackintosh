const REPO="hackintosh-user/VFS5011-hackintosh", CL_BRANCH="active-development";
const esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
function inline(t){return esc(t).replace(/`([^`]+)`/g,"<code>$1</code>").replace(/\*\*([^*]+)\*\*/g,"<b>$1</b>").replace(/\[([^\]]+)\]\((https?:[^)\s]+)\)/g,'<a href="$2" target="_blank" rel="noopener">$1</a>');}
function md(src){
  let out="",list=false,code=false;
  for(const raw of src.split(/\r?\n/)){
    if(raw.startsWith("```")){out+=code?"</pre>":"<pre>";code=!code;continue}
    if(code){out+=esc(raw)+"\n";continue}
    const l=raw.trimEnd();
    if(/^\s*[-*] /.test(l)){if(!list){out+="<ul>";list=true}out+="<li>"+inline(l.replace(/^\s*[-*] /,""))+"</li>";continue}
    if(list){out+="</ul>";list=false}
    if(/^#{1,3} /.test(l)){out+="<h3>"+inline(l.replace(/^#+ /,""))+"</h3>";continue}
    if(/^-{3,}$/.test(l)){out+="<hr>";continue}
    if(l)out+="<p>"+inline(l)+"</p>";
  }
  return out+(list?"</ul>":"")+(code?"</pre>":"");
}
async function loadChangelog(el){
  try{
    const r=await fetch(`https://raw.githubusercontent.com/${REPO}/${CL_BRANCH}/CHANGELOG.md`);
    if(!r.ok)throw 0;
    el.innerHTML=md(await r.text());
  }catch(e){el.innerHTML=`<p class="muted">Couldn't load the changelog right now. <a href="https://github.com/${REPO}/blob/${CL_BRANCH}/CHANGELOG.md">Read it on GitHub</a>.</p>`}
}
function ago(d){const s=(Date.now()-new Date(d))/1e3;for(const[u,n]of[["d",86400],["h",3600],["m",60]])if(s>=n)return Math.floor(s/n)+u+" ago";return "just now"}
async function loadCommits(el){
  try{
    const br=await (await fetch(`https://api.github.com/repos/${REPO}/branches?per_page=20`)).json();
    if(!Array.isArray(br))throw 0;
    let html="";
    for(const b of br){
      const cs=await (await fetch(`https://api.github.com/repos/${REPO}/commits?sha=${encodeURIComponent(b.name)}&per_page=5`)).json();
      if(!Array.isArray(cs))throw 0;
      html+=`<div class="card" style="margin-bottom:14px"><h3><code>${esc(b.name)}</code> <a href="https://github.com/${REPO}/commits/${encodeURIComponent(b.name)}" style="font-weight:400;font-size:.85rem">all commits</a></h3>`+
      cs.map(c=>`<div class="commit"><code><a href="${c.html_url}" target="_blank" rel="noopener">${c.sha.slice(0,7)}</a></code><div>${esc(c.commit.message.split("\n")[0])}<small>${esc(c.commit.author.name)} · ${ago(c.commit.author.date)}</small></div></div>`).join("")+"</div>";
    }
    el.innerHTML=html||"<p class='muted'>No branches found.</p>";
  }catch(e){el.innerHTML=`<p class="muted">Couldn't load commits right now (GitHub may be rate limiting). <a href="https://github.com/${REPO}/commits">See them on GitHub</a>.</p>`}
}
document.querySelectorAll("[data-changelog]").forEach(loadChangelog);
document.querySelectorAll("[data-commits]").forEach(loadCommits);
