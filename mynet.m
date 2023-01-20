while(1)

    t = tcpip('0.0.0.0', 4000, 'NetworkRole', 'server');
    t.InputBufferSize = 200000;
    nchs = 16; % number of channels
    Fs = 44100;
    T = 1/Fs;
    time = 0:T:(t.InputBufferSize/2/nchs)*T-T;
    fprintf('Waiting for incoming connection ...')
    fopen(t);
    fprintf('Connected\n')
    timeout = 30;
    count = 0;
    samplesElapsed = 0;

    while (1)
        if(t.BytesAvailable > 65536)
            fprintf('%d_', t.BytesAvailable);
            data = fread(t, 65536);
            x = bytes_to_int16_to_float(data, nchs);
            samplesElapsed = samplesElapsed + size(x, 1);
            plot(samplesElapsed * T + time(1:size(x, 1)), x)
            xlabel('T[s]')
            ylabel('Amplitude')
            count = 0;
        else
            fwrite(t, 'ping')
            pause(0.1)
            fprintf('.')
            count = count + 1;
            if(count > timeout)
                fprintf('Timeout\n')
                break;
            end
        end
    end
end

function out = bytes_to_int16_to_float(data, nchs)
    nbytes = 2;
    if(mod(size(data, 1), nchs))
        out = nan(1, nchs);
        return
    end
    out = nan(size(data) ./ [nchs * nbytes, 1 / nchs] );
    for c = 0:nchs-1
        lsb = data(c * nbytes + 1 : nchs * nbytes : end);
        msb = data(c * nbytes + 2 : nchs * nbytes : end);
        % msb is signed
        msb(msb > 127) = msb(msb > 127) - 256;
        out(:, c + 1) = msb * 256 + lsb;
    end
    % normalise
    out = out / 32768;
end