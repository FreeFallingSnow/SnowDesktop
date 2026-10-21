import {z} from 'zod';
import {createTypertRemoteInfo} from '@deepseek-ai/dsh-typert-protocol/remote-info';

// The original snapshot remains argument-free. Detailed reads use a Host route allowlist.
export const TYPERT_REMOTE=createTypertRemoteInfo({package:'@local/snowdesktop-collaboration',methods:{
 'snowCollaboration/snapshot':{service:'snowCollaboration',description:'Read the identity-checked local collaboration dashboard snapshot.',invocation:{kind:'direct'},parameters:[],
  result:{mode:'strict',typeSymbol:'@local/snowdesktop-collaboration#snapshot:result',create:()=>z.string(),jsonSchema:{type:'string'}}},
 'snowCollaboration/read':{service:'snowCollaboration',description:'Read an allowlisted batch or retry-log projection from this checkout.',invocation:{kind:'direct'},
  parameters:[{name:'route',source:'json',codec:{mode:'strict',typeSymbol:'@local/snowdesktop-collaboration#read:route',create:()=>z.string(),jsonSchema:{type:'string'}}}],
  result:{mode:'strict',typeSymbol:'@local/snowdesktop-collaboration#read:result',create:()=>z.string(),jsonSchema:{type:'string'}}}
}},z);
export default TYPERT_REMOTE;
