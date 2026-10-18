// bayo.sh: source of the background music (src/bayosh_music.mp4). It plays these steps with Web Audio;
// the .m4a is two loops of it rendered with an OfflineAudioContext (from 58.23 s, 58.18 s long), AAC 112k, muxed with a 16x16 black H.264 track (the PS5 browser only plays it as a video).

/* ---------- music: an original dark-fantasy "shrine" piece made live with Web Audio (no audio file) ---------- */
var AC=null,MG=null,MUS=(function(){try{return localStorage.getItem('bs_music')!=='0'}catch(e){return true}})(),MON=false,MT=null,NX=0,ST=0,NZ=null,RV=null,DR=null;
var BPM=66,S16=60/BPM/2; // one step = an eighth note (name kept for the scheduler)
// D minor, one chord per bar (original progression): Dm Bb F C Gm Dm Bb A
var CH=[[50,53,57,62],[46,50,53,58],[41,48,53,57],[48,52,55,60],[43,50,55,58],[50,53,57,62],[46,50,53,58],[45,52,57,61]],
    BS=[38,34,41,36,43,38,34,33];
// cello line (original): [step in the 8-bar loop, MIDI note, length in eighths]
var MEL=[[0,69,4],[4,65,2],[6,64,2],[8,62,6],[14,65,2],[16,72,4],[20,69,4],[24,67,6],[30,64,2],[32,70,4],[36,69,2],[38,67,2],[40,65,4],[44,64,2],[46,62,2],[48,74,3],[51,72,1],[52,70,4],[56,69,6],[62,73,2]];
function mf(n){return 440*Math.pow(2,(n-69)/12)}
function musInit(){if(AC)return true;var C=window.AudioContext||window.webkitAudioContext;if(!C)return false;try{AC=new C()}catch(e){return false}
 MG=AC.createGain();MG.gain.value=0;MG.connect(AC.destination);
 DR=AC.createGain();DR.gain.value=.55;DR.connect(MG); // dry
 RV=AC.createConvolver();var sr=AC.sampleRate,len=Math.floor(sr*3.2),ir=AC.createBuffer(2,len,sr); // cathedral-ish tail
 for(var c=0;c<2;c++){var d=ir.getChannelData(c);for(var i=0;i<len;i++)d[i]=(Math.random()*2-1)*Math.pow(1-i/len,2.6)}
 RV.buffer=ir;var rg=AC.createGain();rg.gain.value=.65;RV.connect(rg);rg.connect(MG);
 var nl=Math.floor(sr*.6),nb=AC.createBuffer(1,nl,sr),nd=nb.getChannelData(0);for(var k=0;k<nl;k++)nd[k]=Math.random()*2-1;NZ=nb;return true}
function out(node,wet){node.connect(DR);var w=AC.createGain();w.gain.value=wet;node.connect(w);w.connect(RV)}
function strings(t,notes,dur){var f=AC.createBiquadFilter(),g=AC.createGain();f.type='lowpass';f.frequency.value=1100;f.Q.value=.7;
 g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(.055,t+dur*.35);g.gain.setValueAtTime(.055,t+dur*.75);g.gain.linearRampToValueAtTime(0,t+dur*1.05);f.connect(g);out(g,.8);
 notes.forEach(function(n){[-9,9].forEach(function(dt){var o=AC.createOscillator();o.type='sawtooth';o.frequency.value=mf(n);o.detune.value=dt;o.connect(f);o.start(t);o.stop(t+dur*1.1)})})}
function choir(t,notes,dur){var g=AC.createGain(),f1=AC.createBiquadFilter(),f2=AC.createBiquadFilter();f1.type='bandpass';f1.frequency.value=720;f1.Q.value=6;f2.type='bandpass';f2.frequency.value=1150;f2.Q.value=7;
 g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(.16,t+dur*.45);g.gain.linearRampToValueAtTime(0,t+dur*1.05);f1.connect(g);f2.connect(g);out(g,1);
 notes.slice(0,3).forEach(function(n,k){var o=AC.createOscillator(),v=AC.createOscillator(),vg=AC.createGain();o.type='sawtooth';o.frequency.value=mf(n-12);v.frequency.value=4.6+k*.4;vg.gain.value=9;v.connect(vg);vg.connect(o.detune);o.connect(f1);o.connect(f2);[o,v].forEach(function(x){x.start(t);x.stop(t+dur*1.1)})})}
function harp(t,n,v){var o=AC.createOscillator(),o2=AC.createOscillator(),g=AC.createGain(),f=AC.createBiquadFilter();o.type='triangle';o2.type='sine';o.frequency.value=mf(n);o2.frequency.value=mf(n+12);f.type='lowpass';f.frequency.value=2600;
 g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(v,t+.004);g.gain.exponentialRampToValueAtTime(.0001,t+1.6);o.connect(f);o2.connect(f);f.connect(g);out(g,.7);[o,o2].forEach(function(x){x.start(t);x.stop(t+1.7)})}
function bell(t,n){[[1,.09],[2.76,.04],[5.4,.02],[8.9,.01]].forEach(function(p){var o=AC.createOscillator(),g=AC.createGain();o.type='sine';o.frequency.value=mf(n)*p[0];g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(p[1],t+.003);g.gain.exponentialRampToValueAtTime(.0001,t+4.5/p[0]+.6);o.connect(g);out(g,1);o.start(t);o.stop(t+5.2)})}
function drum(t,v){var o=AC.createOscillator(),g=AC.createGain();o.frequency.setValueAtTime(95,t);o.frequency.exponentialRampToValueAtTime(42,t+.5);g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(v,t+.006);g.gain.exponentialRampToValueAtTime(.0001,t+1.4);o.connect(g);out(g,.9);o.start(t);o.stop(t+1.5);
 var n=AC.createBufferSource(),nf=AC.createBiquadFilter(),ng=AC.createGain();n.buffer=NZ;nf.type='lowpass';nf.frequency.value=400;ng.gain.setValueAtTime(v*.5,t);ng.gain.exponentialRampToValueAtTime(.0001,t+.25);n.connect(nf);nf.connect(ng);out(ng,.9);n.start(t);n.stop(t+.3)}
function bassn(t,n,dur){var o=AC.createOscillator(),g=AC.createGain(),f=AC.createBiquadFilter();o.type='sawtooth';o.frequency.value=mf(n-12);f.type='lowpass';f.frequency.value=220;g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(.2,t+.4);g.gain.linearRampToValueAtTime(0,t+dur);o.connect(f);f.connect(g);out(g,.5);o.start(t);o.stop(t+dur+.05)}
function cello(t,n,dur){var o=AC.createOscillator(),o2=AC.createOscillator(),f=AC.createBiquadFilter(),g=AC.createGain(),v=AC.createOscillator(),vg=AC.createGain();
 o.type='sawtooth';o2.type='sawtooth';o.frequency.value=mf(n-12);o2.frequency.value=mf(n-12);o2.detune.value=6;v.frequency.value=5.2;vg.gain.setValueAtTime(0,t);vg.gain.linearRampToValueAtTime(14,t+dur*.6);v.connect(vg);vg.connect(o.detune);vg.connect(o2.detune);
 f.type='lowpass';f.frequency.value=1500;f.Q.value=1.2;g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(.075,t+.18);g.gain.setValueAtTime(.075,t+dur*.85);g.gain.linearRampToValueAtTime(0,t+dur+.25);
 o.connect(f);o2.connect(f);f.connect(g);out(g,.9);[o,o2,v].forEach(function(x){x.start(t);x.stop(t+dur+.3)})}
function musStep(t,i){var loop=Math.floor(i/64),s=i%64,bar=Math.floor(s/8),b=s%8,ch=CH[bar],bd=S16*8;
 if(b==0){strings(t,ch,bd);bassn(t,BS[bar],bd);if(loop%2==1||bar%2==0)choir(t,ch,bd*(bar%2==0?2:1))}
 if(b==0&&bar%4==0)bell(t,ch[3]+12);
 var arp=[0,1,2,3,2,1,3,2];harp(t,ch[arp[b]]+12,b==0?.06:.04);
 if(loop>0&&b==0&&bar%2==0)drum(t,.5);if(loop>0&&bar==7&&(b==4||b==6||b==7))drum(t,.25+.08*(b-4));
 if(loop%2==1)MEL.forEach(function(m){if(m[0]==s)cello(t,m[1],S16*m[2])})}
function musTick(){if(!MON)return;while(NX<AC.currentTime+.2){musStep(NX,ST++);NX+=S16}}
function musStart(){if(MON||!MUS||!musInit())return;try{AC.resume&&AC.resume()}catch(e){}MON=true;NX=AC.currentTime+.08;ST=0;MG.gain.cancelScheduledValues(AC.currentTime);MG.gain.setValueAtTime(MG.gain.value,AC.currentTime);MG.gain.linearRampToValueAtTime(.55,AC.currentTime+4);MT=setInterval(musTick,40);musUi()}
function musStop(){if(!MON)return;MON=false;clearInterval(MT);try{MG.gain.cancelScheduledValues(AC.currentTime);MG.gain.setValueAtTime(MG.gain.value,AC.currentTime);MG.gain.linearRampToValueAtTime(0,AC.currentTime+.8)}catch(e){}musUi()}
function musToggle(){MUS=!MUS;try{localStorage.setItem('bs_music',MUS?'1':'0')}catch(e){}if(MUS){musStart();toast('Música encendida')}else{musStop();toast('Música apagada')}musUi()}
function musUi(){var b=$('mus'),r=$('musrow');if(b)b.className='mus fx'+(MUS&&MON?' on':'');if(r)r.className='row fx'+(MUS?' on':'')}
['keydown','pointerdown','touchstart','click'].forEach(function(ev){document.addEventListener(ev,function(e){var t=e.target;if(t&&t.closest&&t.closest('#mus,#musrow'))return;if(MUS&&!MON)musStart()},{passive:true})});
document.addEventListener('visibilitychange',function(){if(!AC)return;try{document.hidden?AC.suspend():(MON&&AC.resume())}catch(e){}});

