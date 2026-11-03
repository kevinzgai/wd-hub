/* Included by muse_chat_link.c after subscription_event(). No heap allocation. */
enum { J_KEY_END, J_KEY, J_COLON, J_VALUE, J_COMMA_END, J_VALUE_END };
enum { F_SKIP, F_PAYLOAD, F_TYPE, F_SEQ, F_READY, F_EVENT, F_ID, F_ID_ALIAS,
       F_PARENT, F_PARENT_ALIAS, F_DISPLAY, F_CONTENT, F_TEXT };
enum { P_ID=1, P_PARENT=2, P_TEXT=4, P_READY=8 };

static unsigned stream_field(const char *key, unsigned scope)
{
    if (scope == 1) {
        if (!strcmp(key,"payload")) return F_PAYLOAD;
        if (!strcmp(key,"type")) return F_TYPE;
        if (!strcmp(key,"seq")) return F_SEQ;
        if (!strcmp(key,"event") || !strcmp(key,"event_name")) return F_EVENT;
    }
    if (!scope) return F_SKIP;
    if (!strcmp(key,"display_text_ready")) return F_READY;
    if (!strcmp(key,"message_id")) return F_ID;
    if (!strcmp(key,"id")) return F_ID_ALIAS;
    if (!strcmp(key,"reply_to_message_id")) return F_PARENT;
    if (!strcmp(key,"parent_message_id")) return F_PARENT_ALIAS;
    if (!strcmp(key,"display_text")) return F_DISPLAY;
    if (!strcmp(key,"content")) return F_CONTENT;
    if (!strcmp(key,"text")) return F_TEXT;
    return F_SKIP;
}

static void stream_reset(void)
{
    memset(s_mem->stream,0,sizeof(*s_mem->stream));
    memset(&s_row,0,sizeof(s_row));
    s_row.ready=true;
}

static void stream_put(row_stream_t *p, const char *bytes, size_t n)
{
    if (!p->cap || p->clipped) return;
    if (p->used+n >= p->cap) { p->clipped=true; return; }
    memcpy(p->token+p->used,bytes,n); p->used+=n;
}

static void stream_codepoint(row_stream_t *p, uint32_t c)
{
    char b[4]; size_t n;
    if (!c) { if(p->cap) p->bad=true; return; } /* Stored C strings cannot represent NUL. */
    if (c<0x80) {b[0]=(char)c;n=1;}
    else if(c<0x800) {b[0]=0xc0|(c>>6);b[1]=0x80|(c&63);n=2;}
    else if(c<0x10000) {b[0]=0xe0|(c>>12);b[1]=0x80|((c>>6)&63);b[2]=0x80|(c&63);n=3;}
    else {b[0]=0xf0|(c>>18);b[1]=0x80|((c>>12)&63);b[2]=0x80|((c>>6)&63);b[3]=0x80|(c&63);n=4;}
    stream_put(p,b,n);
}

static void stream_string_value(row_stream_t *p, unsigned scope, unsigned f)
{
    if (!scope) return;
    unsigned i=scope-1;
    row_t *r=scope==1 ? &s_row : &p->payload;
    unsigned flag=0;
    switch(f) {
    case F_TYPE: strlcpy(r->type,p->token,sizeof(r->type)); break;
    case F_EVENT: strlcpy(r->event,p->token,sizeof(r->event)); break;
    case F_ID: p->id[i]=true; /* fall through */
    case F_ID_ALIAS:
        if(f==F_ID || !p->id[i]) {strlcpy(r->msg,p->token,sizeof(r->msg));flag=P_ID;} break;
    case F_PARENT: p->parent[i]=true; /* fall through */
    case F_PARENT_ALIAS:
        if(f==F_PARENT || !p->parent[i]) {strlcpy(r->reply_to,p->token,sizeof(r->reply_to));flag=P_PARENT;} break;
    case F_DISPLAY: case F_CONTENT: case F_TEXT: {
        unsigned rank=f==F_DISPLAY ? 3 : f==F_CONTENT ? 2 : 1;
        if(rank>=p->rank[i]) {strlcpy(r->text,p->token,sizeof(r->text));p->rank[i]=rank;flag=P_TEXT;}
        break;
    }
    default: break;
    }
    if(scope==2) p->payload_fields|=flag;
}

static void stream_end_string(row_stream_t *p)
{
    if (p->clipped && p->used) {
        size_t start=p->used-1;
        while(start && ((unsigned char)p->token[start]&0xc0)==0x80) start--;
        unsigned char lead=(unsigned char)p->token[start];
        size_t need=lead<0x80?1:lead<0xe0?2:lead<0xf0?3:4;
        if(p->used-start<need) p->used=start;
    }
    p->token[p->used]=0;
    unsigned d=p->depth-1;
    if(p->key) {
        p->stack[d].field=p->clipped ? F_SKIP : stream_field(p->token,p->stack[d].scope);
        p->stack[d].state=J_COLON;
    } else {
        stream_string_value(p,p->stack[d].scope,p->stack[d].field);
        p->stack[d].state=J_COMMA_END;
    }
    p->mode=0;
}

static bool stream_number(const char *s)
{
    if(*s=='-') s++;
    if(*s=='0') s++;
    else {if(*s<'1'||*s>'9')return false; do{s++;}while(*s>='0'&&*s<='9');}
    if(*s=='.') {s++;if(*s<'0'||*s>'9')return false;do{s++;}while(*s>='0'&&*s<='9');}
    if(*s=='e'||*s=='E') {s++;if(*s=='+'||*s=='-')s++;if(*s<'0'||*s>'9')return false;do{s++;}while(*s>='0'&&*s<='9');}
    return !*s;
}

static void stream_end_primitive(row_stream_t *p)
{
    p->token[p->used]=0;
    bool yes=!strcmp(p->token,"true"), no=!strcmp(p->token,"false"), nil=!strcmp(p->token,"null");
    if(p->clipped || (!yes&&!no&&!nil&&!stream_number(p->token))) {p->bad=true;return;}
    unsigned d=p->depth-1, scope=p->stack[d].scope, f=p->stack[d].field;
    if(scope && f==F_READY && (yes||no)) {
        (scope==1?&s_row:&p->payload)->ready=yes;
        if(scope==2)p->payload_fields|=P_READY;
    }
    if(scope==1 && f==F_SEQ && !nil) {
        uint64_t seq=0;
        for(const char *s=p->token;*s;s++) {
            if(*s<'0'||*s>'9'||seq>(UINT64_MAX-(unsigned)(*s-'0'))/10) {p->bad=true;return;}
            seq=seq*10+(unsigned)(*s-'0');
        }
        s_row.seq=seq;
    }
    p->mode=0;p->stack[d].state=J_COMMA_END;
}

static void stream_begin_string(row_stream_t *p, bool key)
{
    unsigned d=p->depth-1, f=p->stack[d].field;
    p->mode=1;p->key=key;p->used=0;p->escape=0;p->high=0;p->clipped=false;
    if(key) p->cap=p->stack[d].scope?24:0;
    else if(!p->stack[d].scope || f==F_SKIP || f==F_PAYLOAD || f==F_READY || f==F_SEQ) p->cap=0;
    else p->cap=f==F_TYPE?16:f==F_EVENT?24:(f>=F_ID&&f<=F_PARENT_ALIAS)?80:TEXT_MAX;
}

static void stream_byte(row_stream_t *p, unsigned char c)
{
    if(p->bad)return;
    if(p->mode==1) {
        if(p->escape==2) {
            int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
            if(digit<0){p->bad=true;return;}
            p->unicode=p->unicode*16+(unsigned)digit;
            if(++p->digits<4)return;
            uint32_t cp=p->unicode;p->escape=0;
            if(p->high) {
                if(cp<0xdc00||cp>0xdfff){p->bad=true;return;}
                cp=0x10000+((p->high-0xd800)<<10)+(cp-0xdc00);p->high=0;
            } else if(cp>=0xd800&&cp<=0xdbff){p->high=cp;p->escape=3;return;}
            else if(cp>=0xdc00&&cp<=0xdfff){p->bad=true;return;}
            stream_codepoint(p,cp);return;
        }
        if(p->escape==3){if(c!='\\')p->bad=true;else p->escape=4;return;}
        if(p->escape==4){if(c!='u')p->bad=true;else{p->escape=2;p->unicode=0;p->digits=0;}return;}
        if(p->escape==1) {
            p->escape=0;
            if(c=='u'){p->escape=2;p->unicode=0;p->digits=0;return;}
            char b=(char)c;
            if(c=='n')b='\n';else if(c=='r'||c=='t'||c=='b'||c=='f')b=' ';
            else if(c!='"'&&c!='\\'&&c!='/'){p->bad=true;return;}
            stream_put(p,&b,1);return;
        }
        if(c=='\\'){p->escape=1;return;}
        if(c=='"'){stream_end_string(p);return;}
        if(c<0x20){p->bad=true;return;}
        char b=(char)c;stream_put(p,&b,1);return;
    }
    bool ws=c==' '||c=='\t'||c=='\r'||c=='\n';
    if(p->mode==2) {
        if(!ws&&c!=','&&c!='}'&&c!=']') {char b=(char)c;stream_put(p,&b,1);return;}
        stream_end_primitive(p);if(p->bad)return;
    }
    if(ws)return;
    if(p->complete){p->bad=true;return;}
    if(!p->depth) {
        if(c!='{'){p->bad=true;return;}
        p->stack[0].kind='{';p->stack[0].state=J_KEY_END;p->stack[0].scope=1;p->depth=1;return;
    }
    unsigned d=p->depth-1, state=p->stack[d].state;
    if(c=='}'||c==']') {
        bool obj=p->stack[d].kind=='{';
        if((obj?(c!='}'||(state!=J_KEY_END&&state!=J_COMMA_END)):(c!=']'||(state!=J_VALUE_END&&state!=J_COMMA_END)))) {p->bad=true;return;}
        if(!--p->depth)p->complete=true;
        return;
    }
    if(state==J_KEY_END||state==J_KEY) {
        if(c!='"'){p->bad=true;return;}stream_begin_string(p,true);return;
    }
    if(state==J_COLON){if(c!=':')p->bad=true;else p->stack[d].state=J_VALUE;return;}
    if(state==J_COMMA_END) {
        if(c!=','){p->bad=true;return;}
        p->stack[d].state=p->stack[d].kind=='{'?J_KEY:J_VALUE;
        p->stack[d].field=F_SKIP;return;
    }
    if(c=='"'){stream_begin_string(p,false);return;}
    if(c=='{'||c=='[') {
        if(p->depth==sizeof(p->stack)/sizeof(p->stack[0])){p->bad=true;return;}
        unsigned scope=c=='{'&&p->stack[d].scope==1&&p->stack[d].field==F_PAYLOAD?2:0;
        p->stack[d].state=J_COMMA_END;
        d=p->depth++;memset(&p->stack[d],0,sizeof(p->stack[d]));
        p->stack[d].kind=c;p->stack[d].scope=scope;p->stack[d].state=c=='{'?J_KEY_END:J_VALUE_END;return;
    }
    if(c=='-'||(c>='0'&&c<='9')||c=='t'||c=='f'||c=='n') {
        p->mode=2;p->used=0;p->cap=sizeof(p->token);p->clipped=false;
        char b=(char)c;stream_put(p,&b,1);return;
    }
    p->bad=true;
}

static void stream_finish(void)
{
    row_stream_t *p=s_mem->stream;
    if(p->bad||!p->complete||p->depth||p->mode||strcmp(s_row.type,"event"))return;
    if(p->payload_fields&P_ID)strlcpy(s_row.msg,p->payload.msg,sizeof(s_row.msg));
    if(p->payload_fields&P_PARENT)strlcpy(s_row.reply_to,p->payload.reply_to,sizeof(s_row.reply_to));
    if(p->payload_fields&P_TEXT)strlcpy(s_row.text,p->payload.text,sizeof(s_row.text));
    if(p->payload_fields&P_READY)s_row.ready=p->payload.ready;
    subscription_event();
}
