while(1)
    if exist('t')
        % properly close any open socket
        fclose(t)
    end
    t = tcpip('0.0.0.0', 4000, 'NetworkRole', 'server');
    t.InputBufferSize = 8000000; % if buffer usage in the prints below goes above 60%, increase this and blocksize_bytes to avoid dropouts
    nchs = 16; % number of channels
    Fs = 44100;
    oneSecond = nchs * 2 * Fs;
    blocksize_bytes = oneSecond * 1; % number of bytes read at once. Larger blocks give higher latency but they may allow to avoid underruns.
    
    T = 1/Fs;
    time = 0:T:(t.InputBufferSize/2/nchs)*T-T;
    fprintf('Waiting for incoming connection ...')
    fopen(t);
    fprintf('Connected\n')
    timeout = 45;
    count = 0;
    samplesElapsed = 0;
    fullX = nan(0, nchs);
    while(0)
        if(t.BytesAvailable > 1)
           fread(t, t.BytesAvailable);
           pause(0.1);
        end
    end
    while (1)
        if(t.BytesAvailable > blocksize_bytes)
            if(t.BytesAvailable == t.InputBufferSize)
                fprintf(2, 'Input buffer is full. A dropout likely occurred. Reduce in-loop computation and/or increase blocksize_bytes and/or increase t.InputBufferSize')
            end
            fprintf('read: %d, buffer: %d bytes (%.0f%%)\n', blocksize_bytes, t.BytesAvailable, 100 * t.BytesAvailable / t.InputBufferSize);
            data = fread(t, blocksize_bytes);
            x = bytes_to_int16_to_float(data, nchs);
            X = sum(x, 2);
%             fullX = cat(1, fullX, x);
%             samplesElapsed = samplesElapsed + size(x, 1);
%             plot(samplesElapsed * T + time(1:size(x, 1)), x)
%             xlabel('T[s]')
%             ylabel('Amplitude')
            count = 0;
        else
%             fwrite(t, 'ping')
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