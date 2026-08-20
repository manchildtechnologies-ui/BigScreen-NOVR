import React, {useEffect, useState} from 'react';
import {createRoot} from 'react-dom/client';
import App from './App';
import './styles.css';

function BootSplash(){
  const [finished,setFinished]=useState(false);

  useEffect(()=>{
    const fallback=setTimeout(()=>setFinished(true),15000);
    return()=>clearTimeout(fallback);
  },[]);

  if(finished)return <App/>;
  return <main className="boot-splash">
    <video autoPlay muted playsInline onEnded={()=>setFinished(true)} onError={()=>setFinished(true)} src="/logo.mp4"/>
  </main>;
}

createRoot(document.getElementById('root')).render(<React.StrictMode><BootSplash/></React.StrictMode>);
