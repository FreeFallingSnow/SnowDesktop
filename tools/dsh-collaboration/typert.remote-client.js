import {z} from 'zod';
import {createTypertRemoteInfo} from '@deepseek-ai/dsh-typert-protocol/remote-info';

// Fixed, argument-free read projection; this surface cannot mutate the coordinator.
export const TYPERT_REMOTE=createTypertRemoteInfo({package:'@local/snowdesktop-collaboration',methods:{
 'snowCollaboration/snapshot':{service:'snowCollaboration',description:'Read the identity-checked local collaboration dashboard snapshot.',invocation:{kind:'direct'},parameters:[],
  result:{mode:'strict',typeSymbol:'@local/snowdesktop-collaboration#snapshot:result',create:()=>z.string(),jsonSchema:{type:'string'}}}
}},z);
export default TYPERT_REMOTE;
