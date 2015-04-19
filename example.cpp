/*
 *  This file is part of oscprompt - a curses OSC frontend
 *  Copyright Mark McCurry 2014
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <jack/jack.h>
#include <jack/midiport.h>
#include <rtosc/thread-link.h>
#include <rtosc/miditable.h>
#include <rtosc/port-sugar.h>
#include <lo/lo.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <string>
#include <set>
#include "ports.h"

using std::function;
using namespace rtosc;

ThreadLink bToU(1024,1024);
ThreadLink uToB(1024,1024);
void display(const char *str){bToU.write("/display", "s", str);}

struct Oscil
{
    float volume;
    float cents;
    int shape;

    //private data
    float phase;

    static Ports ports;
};

struct Synth
{
    float freq;
    bool  enable;
    Oscil oscil[16];

    static Ports &ports;
} synth;

jack_port_t   *port, *iport;
jack_client_t *client;

void echo(const char *, RtData){
    bToU.raw_write(uToB.peak());
}

#define rObject Oscil
Ports Oscil::ports = {
    rParamF(cents, rLinear(-1200, 1200), "Detune in cents"),
    rParamF(volume, rLinear(0.0, 1.0), "Volume on linear scale"),
    rParamI(shape,  rMap(max, 2), "Shape of Oscillator: {sine, saw, square}")
};
#undef rObject

void help(msg_t,RtData)
{
    display("Welcome to the OSC prompt, where simple OSC messages control "
            "parameters in a less than simple manner.\n"
            "\n"
            "This application is a simple additive synthesis engine. "
            "The synthesizer ports are:\n"
            "/synth/enable, /synth/oscil#/cents, /synth/oscil#/volume, /synth/oscil#/shape, "
            "/synth/freq\n"
            "For some audio enable the output, make one volume non-zero, and set a frequency\n\n"
            "/synth/enable T\n"
            "/synth/oscil0/volume 0.2\n"
            "/synth/freq 440.0\n\n"
            "Good Luck...");
}

#define rObject Synth
Ports synth_ports = {
    rParamF(freq, rLinear(0, 20e3), "Base frequency of note"),
    PARAMT(Synth, enable, enable, "Enable or disable audio output"),
    rRecurs(oscil, 16, "Oscillator bank element")
};
Ports &Synth::ports = synth_ports;

void apropos(msg_t m, RtData);
void describe(msg_t m, RtData);
void midi_register(msg_t m, RtData);
bool do_exit = false;

Ports ports = {
    //Meta port
    {"echo",             ":hidden\0" rDoc("Echo all parameters back"), 0, echo},
    {"help:",            rDoc("Display help to user"),                 0, help},
    {"apropos:s",        rDoc("Find the best match"),                  0, apropos},
    {"describe:s",       rDoc("Print out a description of a port"),    0, describe},
    {"midi-register:is", rDoc("Register a midi port <ctl id, path>"),  0, midi_register},
    {"quit:",            rDoc("Quit the program"), 0,
        [](msg_t, RtData){do_exit=true; bToU.write("/disconnect","");}},

    //Normal ports
    {"synth/", rDoc("Main ports for synthesis"), &Synth::ports,
        [](msg_t m, RtData &d){d.obj = &synth; Synth::ports.dispatch(snip(m), d); }},
};


Ports *backend_ports = &ports;


void apropos(msg_t m, RtData)
{
    const char *s = rtosc_argument(m,0).s;
    if(*s=='/') ++s;
    const Port *p = ports.apropos(s);
    if(p)
        display(p->name);
    else
        display("unknown path...");
}

void describe(msg_t m, RtData)
{
    const char *s = rtosc_argument(m,0).s;
    const char *ss = rtosc_argument(m,0).s;
    if(*s=='/') ++s;
    const Port *p = ports.apropos(s);
    if(p)
        display(p->metadata);
    else
        bToU.write("/display", "sss", "could not find path...<", ss, ">");
}

#define PI 3.14159f

inline float warp(unsigned shape, float phase)
{
    if(shape == 0)
        return phase;
    else if(shape == 1)
        return sinf(2*PI*phase);
    else if(shape == 2)
        return phase<0.5?-1:1;

    return 0.0f;
}

MidiTable midi(*backend_ports);

//Note stack for mono playing
static char notes[16];
//static char current_note = 0;
void push_note(char note)
{
    synth.freq = 440.0f * powf(2.0f, (note-69.0f)/12.0f);

    //no duplicates
    for(int i=0; i<16; ++i)
        if(notes[i]==note)
            return;

    for(int i=15;i>=0; --i)
        notes[i+1] = notes[i];
    notes[0] = note;

    //current_note = ev.buffer[1];
    synth.enable = 1;
}
void pop_note(char note)
{
    int note_pos=-1;
    for(int i=0; i<16; ++i)
        if(notes[i]==note)
            note_pos = i;
    if(note_pos != -1) {
        for(int i=note_pos; i<15; ++i)
            notes[i] = notes[i+1];
        notes[15] = 0;
    }

    if(!notes[0])
        synth.enable = 0;
    else
        synth.freq = 440.0f * powf(2.0f, (notes[0]-69.0f)/12.0f);
}

void midi_register(msg_t m, RtData)
{
    midi.addElm(0,
            rtosc_argument(m,0).i,
            rtosc_argument(m,1).s);
}

class RtPass:public RtData
{
    public:
        void reply(const char *path, const char *args, ...)
        {
            va_list va;
            va_start(va,args);
            const size_t len =
                rtosc_vmessage(bToU.buffer(),bToU.buffer_size(),path,args,va);
            if(len)
                bToU.raw_write(bToU.buffer());
        }

        void reply(const char *msg)
        {
            bToU.raw_write(msg);
        }
};

int process(unsigned nframes, void*)
{
    char loc_buf[1024];
    memset(loc_buf, 0, sizeof(loc_buf));
    RtPass d;
    d.loc = loc_buf;
    d.loc_size = 1024;
    d.obj = NULL;
    //Handle user events
    while(uToB.hasNext()) {
        d.matches = 0;
        const char *msg = uToB.read();
        ports.dispatch(msg+1, d);
    }

    //Handle midi events
    void *midi_buf = jack_port_get_buffer(iport, nframes);
    jack_midi_event_t ev;
    jack_nframes_t event_idx = 0;
    while(jack_midi_event_get(&ev, midi_buf, event_idx++) == 0) {
        switch(ev.buffer[0]&0xf0) {
            case 0x90: //Note On
                push_note(ev.buffer[1]);
                break;
            case 0x80: //Note Off
                pop_note(ev.buffer[1]);
                break;
            case 0xB0: //Controller
                midi.process(ev.buffer[0]&0x0f, ev.buffer[1], ev.buffer[2]);
                break;
        }
    }

    //Setup jack parameters
    const float Fs = jack_get_sample_rate(client);
    float *output  = (float*) jack_port_get_buffer(port, nframes);

    //Zero out buffer
    for(unsigned i=0; i<nframes; ++i)
        output[i] = 0.0f;

    //Don't synthesize anything if the output is disabled
    if(!synth.enable)
        return 0;

    //Gather all oscilators
    for(int i=0; i<16; ++i) {
        float &phase  = synth.oscil[i].phase;
        float  volume = synth.oscil[i].volume;
        int    shape  = synth.oscil[i].shape;
        float  nfreq  = synth.freq*powf(2.0f,synth.oscil[i].cents/1200.0f);
        const float incf = nfreq/Fs;//oscil[0].freq/Fs;

        for(unsigned j=0; j<nframes; ++j) {
            output[j] += volume*warp(shape, phase);

            phase     += incf;
            if(phase>1.0f)
                phase -= 1.0f;
        }
    }
    return 0;
}

void cleanup_audio(void)
{
    jack_deactivate(client);
    jack_client_close(client);
}

void init_audio(void)
{
    //setup miditable
    midi.event_cb = [](const char *m){
        char buffer[1024];
        memset(buffer,0,sizeof(buffer));
        RtData d;
        d.loc = buffer;
        d.loc_size = 1024;
        ports.dispatch(m+1, d);};
    midi.error_cb = [](const char *m1, const char *m2)
    {bToU.write("/error", "ss", m1, m2);};

    //Setup ports
    client = jack_client_open("oscprompt-demo", JackNullOption, NULL, NULL);
    jack_set_process_callback(client, process, NULL);

    port = jack_port_register(client, "output",
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);

    iport = jack_port_register(client, "input",
            JACK_DEFAULT_MIDI_TYPE, JackPortIsInput | JackPortIsTerminal, 0);

    //Run audio
    jack_activate(client);
    atexit(cleanup_audio);
}


void error(int i, const char *m, const char *loc)
{
    fprintf(stderr, "%d-%s@%s\n",i,m,loc);
}


std::string last_url;//last url to be sent down the pipe
std::string curr_url;//last url to pop out of the pipe

std::set<std::string> logger;

void path_search(msg_t m)
{
    //assumed upper bound of 32 ports (may need to be resized)
    char         types[65];
    rtosc_arg_t  args[64];
    size_t       pos    = 0;
    const Ports *ports  = NULL;
    const char  *str    = rtosc_argument(m,0).s;
    const char  *needle = rtosc_argument(m,1).s;

    //zero out data
    memset(types, 0, sizeof(types));
    memset(args,  0, sizeof(args));

    if(!*str) {
        ports = &::ports;
    } else {
        const Port *port = ::ports.apropos(rtosc_argument(m,0).s);
        if(port)
            ports = port->ports;
    }

    if(ports) {
        //RTness not confirmed here
        for(const Port &p:*ports) {
            if(strstr(p.name, needle)!=p.name)
                continue;
            types[pos]    = 's';
            args[pos++].s = p.name;
            types[pos]         = 'b';
            args[pos].b.len    = p.meta().length();
            args[pos++].b.data = (uint8_t*) p.metadata;
        }
    }

    //Reply to requester
    char buffer[1024];

    size_t length = rtosc_amessage(buffer, 1024, "/paths", types, args);

    if(length) {
        lo_message msg  = lo_message_deserialise((void*)buffer, length, NULL);
        lo_address addr = lo_address_new_from_url(last_url.c_str());
        if(addr)
            lo_send_message(addr, buffer, msg);
    }
}

int handler_function(const char *path, const char *, lo_arg **, int, lo_message msg, void *)
{
    lo_address addr = lo_message_get_source(msg);
    if(addr) {
        const char *tmp = lo_address_get_url(addr);
        if(tmp != last_url) {
            uToB.write("/echo", "ss", "OSC_URL", tmp);
            last_url = tmp;
        }

    }

    char buffer[2048];
    memset(buffer, 0, sizeof(buffer));
    size_t size = 2048;
    lo_message_serialise(msg, path, buffer, &size);
    if(!strcmp(buffer, "/logging-start")) {
        if(addr)
            logger.insert(lo_address_get_url(addr));
    } else if(!strcmp(buffer, "/logging-stop")) {
        if(addr)
            logger.erase(lo_address_get_url(addr));
    } else if(!strcmp(buffer, "/path-search") && !strcmp("ss", rtosc_argument_string(buffer))) {
        path_search(buffer);
    } else
        uToB.raw_write(buffer);

    return 0;
}

int main()
{
    init_audio();

    //setup liblo link
    lo_server server = lo_server_new_with_proto(NULL, LO_UDP, error);
    lo_server_add_method(server, NULL, NULL, handler_function, NULL);

    printf("Synth running on port %d\n", lo_server_get_port(server));

    while(!do_exit) {
        lo_server_recv_noblock(server, 100);
        while(bToU.hasNext()) {
            const char *rtmsg = bToU.read();
            if(!strcmp(rtmsg, "/echo")
                    && !strcmp(rtosc_argument_string(rtmsg),"ss")
                    && !strcmp(rtosc_argument(rtmsg,0).s, "OSC_URL"))
                curr_url = rtosc_argument(rtmsg,1).s;
            else {
                lo_message msg  = lo_message_deserialise((void*)rtmsg, rtosc_message_length(rtmsg, bToU.buffer_size()), NULL);
                for(std::string str : logger) {
                    lo_address addr = lo_address_new_from_url(str.c_str());
                    lo_send_message(addr, rtmsg, msg);
                }
                if(logger.find(curr_url) == logger.end()) {
                    lo_address addr = lo_address_new_from_url(curr_url.c_str());
                    lo_send_message(addr, rtmsg, msg);
                }
            }
        }
    }
    return 0;
}

//TODO get broadcasting to work properly
