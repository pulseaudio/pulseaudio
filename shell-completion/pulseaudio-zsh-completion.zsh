#compdef pulseaudio pactl pacmd pacat paplay parecord padsp pasuspender

_devices() {
    local -a _device_list
    local cmd _device _device_description _remote_cmd

    if [[ $service == pactl  || $service == pacmd ]]; then
        case $words[$((CURRENT - 1))] in
            set-sink-input-*) cmd=('sink-inputs');;
            set-sink-*) cmd=('sinks');;
            set-source-output-*) cmd=('source-outputs');;
            set-source-*) cmd=('sources');;
            suspend-sink) cmd=('sinks');;
            suspend-source) cmd=('sources');;
            move-sink-input) cmd=('sink-inputs');;
            move-source-output) cmd=('source-outputs');;
            kill-sink-input) cmd=('sink-inputs');;
            kill-source-output) cmd=('source-outputs');;
        esac

        case $words[$((CURRENT - 2))] in
            move-sink-input) cmd=('sinks');;
            move-source-output) cmd=('sources');;
        esac

    elif [[ $service == (pacat|paplay|parecord) ]]; then
        case $words[$((CURRENT))] in
            --device=*)
                if [[ $words == *(--playback|-p)[[:space:]]* ||
                    $service == paplay ]]; then
                    cmd=('sinks')
                elif [[ $words == *(--record|-r)[[:space:]]* ||
                    $service == parecord ]]; then
                    cmd=('sources')
                else
                    cmd=('sinks' 'sources')
                fi
                ;;
            --monitor-stream=*) cmd=('sink-inputs');;
        esac

        case $words[$((CURRENT - 1))] in
            -d)
                if [[ $words == *(--playback|-p)[[:space:]]* ||
                    $service == paplay ]]; then
                    cmd=('sinks')
                elif [[ $words == *(--record|-r)[[:space:]]* ||
                    $service == parecord ]]; then
                    cmd=('sources')
                else
                    cmd=('sinks' 'sources')
                fi
                ;;
        esac

    fi

    for (( i = 0; i < ${#words[@]}; i++ )) do
        if [[ ${words[$i]} == -s ]]; then
            _remote_cmd="-s ${words[$i+1]}"
            break;
        fi
    done

    for target in $cmd; do
        for device_info in ${(ps:\n\n:)"$(_call_program device_tag "pactl $_remote_cmd list $target 2> /dev/null")"}; do
            for line in ${(f)device_info}; do
                if [[ $target == (sink-inputs|source-outputs) ]]; then
                    if [[ $line == (Sink*Input|Source*Output)* ]]; then
                        _device=${line#*\#}
                    elif [[ $line == *application.name* ]]; then
                        _device_description=${line#*= }
                    fi

                else
                    if [[ $words[$((CURRENT - 1))] == *set-sink-formats* ]]; then
                        if [[ $line == Sink* ]]; then
                            _device=${line#*\#}
                        elif [[ $line == *Description:* ]]; then
                            _device_description=${line#*: }
                        fi

                    else
                        if [[ $line == *Name:* ]]; then
                            _device=${line#*: }
                        elif [[ $line == *Description:* ]]; then
                            _device_description=${line#*: }
                        fi
                    fi
                fi
            done
            _device_list+=($_device:$_device_description)
        done
    done

    _describe 'device list' _device_list
}

_profiles() {
    local -a _profile_list
    local _current_card _raw_profiles _profile_name _profile_description _remote_cmd

    _current_card=$words[$((CURRENT - 1))]

    for (( i = 0; i < ${#words[@]}; i++ )) do
        if [[ ${words[$i]} == -s ]]; then
            _remote_cmd="-s ${words[$i+1]}"
            break;
        fi
    done

    for card in ${(ps:\n\n:)"$(_call_program profiles_tag "pactl $_remote_cmd list cards 2> /dev/null")"}; do
        if [[ $card == *$_current_card* ]]; then
            _raw_profiles=${card##*Profiles:}
            _raw_profiles=${_raw_profiles%%Active Profile:*}
            for profile in ${(f)_raw_profiles}; do
                if [[ $profile != [[:blank:]] ]]; then
                    _profile_name=${profile%%: *}
                    _profile_name=${_profile_name//[[:blank:]]/}
                    _profile_name=${_profile_name//:/\\:}
                    _profile_description=${profile#*: }
                    _profile_list+=($_profile_name:$_profile_description)
                fi
            done
        fi
    done

    _describe 'profile list' _profile_list
}

_ports() {
    local -a _port_list
    local _raw_ports _port_name _port_description _current_device _remote_cmd

    case $words[$((CURRENT - 2))] in
        set-sink-port) cmd="sinks";;
        set-source-port) cmd="sources";;
        set-port-latency-offset) cmd="cards";;
    esac

    _current_device=$words[$((CURRENT - 1))]

    for (( i = 0; i < ${#words[@]}; i++ )) do
        if [[ ${words[$i]} == -s ]]; then
            _remote_cmd="-s ${words[$i+1]}"
            break;
        fi
    done

    for device in ${(ps:\n\n:)"$(_call_program port_tag "pactl $_remote_cmd list $cmd 2> /dev/null")"}; do
        if [[ $device == *Ports:* && $device == *$_current_device* ]]; then
            _raw_ports=${device##*Ports:}
            _raw_ports=${_raw_ports%%Active Port:*}
            for line in ${(f)_raw_ports}; do
                if [[ $line != [[:blank:]] &&
                    $line != (*Part?of*|*Properties:*|*device.icon_name*) ]]; then
                    _port_name=${line%%: *}
                    _port_name=${_port_name//[[:blank:]]/}
                    _port_description=${line#*: }
                    _port_list+=($_port_name:$_port_description)
                fi
            done
        fi
    done

    _describe 'port list' _port_list
}

_cards(){
    local -a _card_list
    local _card _cad_name _remote_cmd

    for (( i = 0; i < ${#words[@]}; i++ )) do
        if [[ ${words[$i]} == -s ]]; then
            _remote_cmd="-s ${words[$i+1]}"
            break;
        fi
    done

    for card_info in ${(ps:\n\n:)"$(_call_program card_tag "pactl $_remote_cmd list cards 2> /dev/null")"}; do
        for line in ${(f)card_info}; do
            if [[ $line == *Name:* ]]; then
                _card=${line#*: }
            elif [[ $line == *alsa.long_card_name* ]]; then
                _card_name=${line#*= \"}
                _card_name=${_card_name%at*}
            fi
        done
        _card_list+=($_card:$_card_name)
    done

    _describe 'card list' _card_list
}

_all_modules(){
    local -a _all_modules_list
    for module in ${(f)"$(_call_program modules_tag "pulseaudio --dump-modules 2> /dev/null")"}; do
        _all_modules_list+=${module%% *}
    done
    _describe 'module list' _all_modules_list
}

_loaded_modules(){
    local -a _loaded_modules_list _remote_cmd

    for (( i = 0; i < ${#words[@]}; i++ )) do
        if [[ ${words[$i]} == -s ]]; then
            _remote_cmd="-s ${words[$i+1]}"
            break;
        fi
    done

    for module in ${(f)"$(_call_program modules_tag "pactl $_remote_cmd list modules short 2> /dev/null")"}; do
        _loaded_modules_list+=(${${(ps:\t:)module}[1]}:${${(ps:\t:)module}[2]})
    done
    _describe 'module list' _loaded_modules_list
}

_resample_methods() {
    local -a _resample_method_list
    for method in ${(f)"$(_call_program modules_tag "pulseaudio --dump-resample-methods 2> /dev/null")"}; do
        _resample_method_list+=$method
    done
    _describe 'resample method list' _resample_method_list
}

_clients() {
    local -a _client_list
    local _client _client_description _remote_cmd

    for (( i = 0; i < ${#words[@]}; i++ )) do
        if [[ ${words[$i]} == -s ]]; then
            _remote_cmd="-s ${words[$i+1]}"
            break;
        fi
    done

    for client_info in ${(ps:\n\n:)"$(_call_program clients_tag "pactl $_remote_cmd list clients 2> /dev/null")"}; do
        for line in ${(f)client_info}; do
            if [[ $line == Client[[:space:]]#* ]]; then
                _client=${line#*\#}
            elif [[ $line == *application.name* ]]; then
                _client_description=${line#*=}
            fi
        done
        _client_list+=($_client:$_client_description)
    done
    _describe 'client list' _client_list
}

_pacat_file_formats() {
    local -a _file_format_list
    for format in ${(f)"$(_call_program fformats_tag "pacat --list-file-formats")"}; do
        _file_format_list+=(${${(ps:\t:)format}[1]}:${${(ps:\t:)format}[2]})
    done
    _describe 'file format list' _file_format_list
}

_pactl_completion() {
    _pactl_command(){
        _pactl_commands=(
            'help: show help and exit'
            'stat: dump statistics about the PulseAudio daemon'
            'info: dump info about the PulseAudio daemon'
            'list: list modules/sources/streams/cards etc...'
            'exit: ask the PulseAudio daemon to exit'
            'upload-sample: upload a sound from a file into the sample cache'
            'play-sample: play the specified sample from the sample cache'
            'remove-sample: remove the specified sample from the sample cache'
            'load-module: load a module'
            'unload-module: unload a module'
            'move-sink-input: move a stream to a sink'
            'move-source-output: move a recording stream to a source'
            'suspend-sink: suspend or resume a sink'
            'suspend-source: suspend or resume a source'
            'set-card-profile: set a card profile:cards:_cards'
            'set-sink-default: set the default sink'
            'set-source-default: set the default source'
            'set-sink-port: set the sink port of a sink'
            'set-source-port: set the source port of a source'
            'set-port-latency-offset: set a latency offset on a port'
            'set-sink-volume: set the volume of a sink'
            'set-source-volume: set the volume of a source'
            'set-sink-input-volume: set the volume of a stream'
            'set-source-output-volume: set the volume of a recording stream'
            'set-sink-mute: mute a sink'
            'set-source-mute: mute a source'
            'set-sink-input-mute: mute a stream'
            'set-source-output-mute: mute a recording stream'
            'set-sink-formats: set supported formats of a sink'
            'subscribe: subscribe to events'
        )
        _describe 'pactl commands' _pactl_commands
    }

    _pactl_list_commands=(
        'modules: list loaded modules'
        'sinks: list available sinks'
        'sources: list available sources'
        'sink-inputs: list connected sink inputs'
        'source-outputs: list connected source outputs'
        'clients: list connected clients'
        'samples: list samples'
        'cards: list available cards'
    )

    _arguments -C \
        - '(help)' \
            {-h,--help}'[display this help and exit]' \
        '--version[show version and exit]' \
        - '(server)' \
            {-s,--server}'[name of server to connect to]:host:_hosts' \
        - '(name)' \
            {-n,--client-name}'[client name to use]:name' \
        '::pactl commands:_pactl_command' \

    case $words[$((CURRENT - 1))] in
        list) _describe 'pactl list commands' _pactl_list_commands;;
        stat) compadd short;;
        set-card-profile) _cards;;
        set-sink-*) _devices;;
        set-source-*) _devices;;
        upload-sample) _files;;
        load-module) _all_modules;;
        unload-module) _loaded_modules;;
        suspend-*) _devices;;
        move-*) _devices;;
        set-port-latency-offset) _cards;;
    esac

    case $words[$((CURRENT - 2))] in
        set-card-profile) _profiles;;
        set-(sink|source)-port) _ports;;
        set-port-latency-offset) _ports;;
        set-*-mute) compadd true false toggle;;
        suspend-*) compadd true false;;
        list) compadd short;;
        move-*) _devices;;
        '-s' | '-n') _pactl_command;;
        --server | --client-*) _pactl_command;;
    esac
}

_pacmd_completion() {
    _pacmd_command(){
        _pacmd_commands=(
            'help: show help and exit'
            'list-modules: list modules'
            'list-cards: list cards'
            'list-sinks: list sinks'
            'list-sources: list sources'
            'list-clients: list clients'
            'list-sink-inputs: list sink-inputs'
            'list-source-outputs: list source-outputs'
            'stat: dump statistics about the PulseAudio daemon'
            'info: dump info about the PulseAudio daemon'
            'load-module: load a module'
            'unload-module: unload a module'
            'describe-module: print info for a module'
            'set-sink-volume: set the volume of a sink'
            'set-source-volume: set the volume of a source'
            'set-sink-mute: mute a sink'
            'set-source-mute: mute a source'
            'set-sink-input-volume: set the volume of a stream'
            'set-source-output-volume: set the volume of a recording stream'
            'set-sink-input-mute: mute a stream'
            'set-source-output-mute: mute a recording stream'
            'set-default-sink: set the default sink'
            'set-default-source: set the default source'
            'set-card-profile: set a card profile'
            'set-sink-port: set the sink port of a sink'
            'set-source-port: set the source port of a source'
            'set-port-latency-offset: set a latency offset on a port'
            'suspend-sink: suspend or resume a sink'
            'suspend-source: suspend or resume a source'
            'suspend: suspend all sinks and sources'
            'move-sink-input: move a stream to a sink'
            'move-source-output: move a recording stream to a source'
            'update-sink-proplist: update the properties of a sink'
            'update-source-proplist: update the properties of a source'
            'update-sink-input-proplist: update the properties of a sink-input'
            'update-source-output-proplist: update the properties of a source-output'
            'list-samples: list samples'
            'play-sample: play the specified sample from the sample cache' # TODO
            'remove-sample: remove the specified sample from the sample cache' # TODO
            'load-sample: upload a sound from a file into the sample cache'
            'load-sample-lazy: lazily upload a sound file into the sample cache'
            'load-sample-dir-lazy: lazily upload all sound files in a directory into the sample cache'
            'kill-client: kill a client'
            'kill-sink-input: kill a sink input'
            'kill-source-output: kill a source output'
            'set-log-target: change the log target'
            'set-log-level: change the log level'
            'set-log-meta: show source code location in log messages'
            'set-log-time: show timestamps in log messages'
            'set-log-backtrace: show backtrace in log messages'
            'play-file: play a sound file'
            'dump: show daemon configuration'
            'dump-volumes: show the state of all volumes'
            'shared: show shared properties'
            'exit: ask the PulseAudio daemon to exit'
        )
        _describe 'pacmd commands' _pacmd_commands
    }

    _arguments -C \
        - '(help)' \
            {-h,--help}'[display this help and exit]' \
        '--version[show version and exit]' \
        '::pacmd commands:_pacmd_command' \

    case $words[$((CURRENT - 1))] in
        set-card-profile) _cards;;
        set-sink-*) _devices;;
        set-source-*) _devices;;
        load-module) _all_modules;;
        describe-module) _all_modules;;
        unload-module) _loaded_modules;;
        suspend-*) _devices;;
        move-*) _devices;;
        set-port-latency-offset) _cards;;
        load-sample*) _files;;
        kill-client) _clients;;
        kill-(sink|source)-*) _devices;;
        set-log-target) compadd null auto syslog stderr file:;;
        set-log-*) compadd true false;;
        play-file) _files;;
    esac

    case $words[$((CURRENT - 2))] in
        set-card-profile) _profiles;;
        set-(sink|source)-port) _ports;;
        set-port-latency-offset) _ports;;
        set-*-mute) compadd true false;;
        suspend-*) compadd true false;;
        move-*) _devices;;
    esac
}

_pasuspender_completion() {
    _arguments -C \
        {-h,--help}'[display this help and exit]' \
        '--version[show version and exit]' \
        {-s,--server}'[name of server to connect to]:host:_hosts' \
}

_padsp_completion() {
    _arguments -C \
        '-h[display this help and exit]' \
        '-s[name of server to connect to]:host:_hosts' \
        '-n[client name to use]:name:' \
        '-m[stream name to use]:name:' \
        '-M[disable /dev/mixer emulation]' \
        '-S[disable /dev/sndstat emulation]' \
        '-D[disable /dev/dsp emulation]' \
        '-d[enable debug output]' \
        '--[disable further command line parsing]' \
}

# TODO channel map completion
_pacat_completion() {
    _pacat_sample_formats=('s16le' 's16be' 'u8' 'float32le' 'float32be'
        'ulaw' 'alaw' 's32le' 's32be' 's24le' 's24-32le' 's24-32be')

    _arguments -C \
        {-h,--help}'[display this help and exit]' \
        '--version[show version and exit]' \
        {-r,--record}'[create a connection for recording]' \
        {-p,--playback}'[create a connection for playback]' \
        {-s,--server=}'[name of server to connect to]:host:_hosts' \
        {-d,--device=}'[name of sink/source to connect to]:device:_devices' \
        '--monitor-stream=[index of the sink input to record from]:device:_devices' \
        {-n,--client-name=}'[client name to use]:name' \
        '--stream-name=[how to call this stream]:name' \
        '--volume=[initial volume to use]:volume' \
        '--rate=[sample rate to use]:rate:(44100 48000 96000)' \
        '--format=[sample type to use]:format:((${(q)_pacat_sample_formats}))' \
        '--channels=[number of channels to use]:number:(1 2)' \
        '--channel-map=[channel map to use]:map' \
        '--fix-format[use the sample format of the sink]' \
        '--fix-rate[use the rate of the sink]' \
        '--fix-channels[channel map of the sink]' \
        '--no-remix[do not upmix or downmix channels]' \
        '--no-remap[map channels by index instead of name]' \
        '--latency=[request the specified latency]:bytes' \
        '--process-time=[request the specified process time]:bytes' \
        '--latency-msec=[request the specified latency in msec]:msec' \
        '--process-time-msec=[request the specified process time in msec]:msec' \
        '--property=[set the specified property]:property' \
        '--raw[record/play raw PCM data]' \
        '--passthrough[passtrough data]' \
        '--file-format=[record/play formatted PCM data]:format:_pacat_file_formats' \
        '--list-file-formats[list available formats]' \
        '::files:_files' \
}

# TODO log-target file completion
_pulseaudio_completion() {
    _arguments -C \
        {-h,--help}'[display this help and exit]' \
        '--version[show version and exit]' \
        '--dump-conf[show default configuration]' \
        '--dump-modules[show available modules]' \
        '--dump-resample-methods[show available resample methods]' \
        '--cleanup-shm[cleanup shared memory]' \
        '--start[start the daemon]' \
        {-k,--kill}'[kill a running daemon]' \
        '--check[check for a running daemon]' \
        '--system=[run as systemd-wide daemon]:bool:(true false)' \
        {-D,--daemonize=}'[daemonize after startup]:bool:(true false)' \
        '--fail=[quit when startup fails]:bool:(true false)' \
        '--high-priority=[try to set high nice level]:bool:(true false)' \
        '--realtime=[try to enable rt scheduling]:bool:(true false)' \
        '--disallow-module-loading=[disallow module loading]:bool:(true false)' \
        '--disallow-exit=[disallow user requested exit]' \
        '--exit-idle-time=[terminate the daemon on passed idle time]:time' \
        '--scache-idle-time=[unload autoloaded samples on passed idle time]:time' \
        '--log-level=[set the verbosity level]:level' \
        '-v[increase the verbosity level]' \
        '--log-target=[set the log target]:target:(auto syslog stderr file\: new_file\:):file' \
        '--log-meta=[include code location in log messages]:bool:(true false)' \
        '--log-time=[include timestamps in log messages]:bool:(true false)' \
        '--log-backtrace=[include backtrace in log messages]:frames' \
        {-p,--dl-search-path=}'[set the search path for plugins]:dir:_files' \
        '--resample-method=[set the resample method]:method:_resample_methods' \
        '--use-pid-file=[create a PID file]:bool:(true false)' \
        '--no-cpu-limit=[do not install CPU load limiter]:bool:(true false)' \
        '--disable-shm=[disable shared memory support]:bool:(true false)' \
        {-L,--load=}'[load the specified module]:modules:_all_modules' \
        {-F,--file=}'[run the specified script]:file:_files' \
        '-C[open a command line on the running tty]' \
        '-n[do not load the default script file]' \
}

_pulseaudio() {
    local state line curcontext="$curcontext"

    case $service in
        pulseaudio) _pulseaudio_completion;;
        pactl) _pactl_completion;;
        pacmd) _pacmd_completion;;
        pacat) _pacat_completion;;
        paplay)_pacat_completion;;
        parecord)_pacat_completion;;
        padsp) _padsp_completion;;
        pasuspender) _pasuspender_completion;;
        *) _message "Err";;
    esac
}

_pulseaudio "$@"

#vim: set ft=zsh sw=4 ts=4 noet
