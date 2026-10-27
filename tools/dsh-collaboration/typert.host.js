import {TYPERT_REMOTE} from './typert.remote-client.js';
export const TYPERT={package:'@local/snowdesktop-collaboration',face:'host',schemas:[],get invocations(){return TYPERT_REMOTE.descriptors;},model:{
 services:[{key:'snowCollaboration',exportName:'apply',description:'Read identity-checked collaboration state from the configured local checkout. Created by the exported apply factory.',tags:[],
  members:[{kind:'method',name:'snapshot',signature:'async snapshot(): Promise<string>',summary:'Read the current collaboration snapshot.'},{kind:'method',name:'read',signature:'async read(route: string): Promise<string>',summary:'Read a fixed batch, retry-log or Run-log route after validating the listener identity.'}],types:[]}],events:[],objects:[]
}};
