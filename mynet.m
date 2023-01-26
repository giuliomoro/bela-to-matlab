while(1) % this loop allows automatic reconnecting in case of timeout
    if exist('t', 'var') && isa(socket, 'tcpip')
        % properly close any open socket
        fclose(socket);
    end
    socket = tcpip('0.0.0.0', 4000, 'NetworkRole', 'server');
    socket.InputBufferSize = 8000000; % if buffer usage in the prints below goes above 60%, increase this and blocksize_bytes to avoid dropouts
    nchs = 16; % number of channels. This must match the one on Bela
    Fs = 44100; % sampling rate. This must match the one on Bela
    oneSecond = nchs * 2 * Fs;
    blocksize_bytes = oneSecond * 1; % number of bytes read at once. Larger blocks give higher latency but they may allow to avoid underruns.
    
    T = 1/Fs;
    time = 0:T:(socket.InputBufferSize/2/nchs)*T-T;
    fprintf('Waiting for incoming connection ...')
    fopen(socket);
    fprintf('Connected\n')
    timeout = 60;
    count = 0;
    samplesElapsed = 0;
    fullX = nan(0, nchs);
    while (1)
        if(socket.BytesAvailable > blocksize_bytes)
            if(socket.BytesAvailable == socket.InputBufferSize)
                fprintf(2, 'Input buffer is full. A dropout likely occurred. Reduce in-loop computation and/or increase blocksize_bytes and/or increase t.InputBufferSize')
            end
            fprintf('read: %d, buffer: %d bytes (%.0f%%)\n', blocksize_bytes, socket.BytesAvailable, 100 * socket.BytesAvailable / socket.InputBufferSize);
            data = fread(socket, blocksize_bytes);
            x = bytes_to_int16_to_float(data, nchs);
            samplesElapsed = samplesElapsed + size(x, 1);
            % write your real-time processing code here. If you see "Input buffer is full" errors it probably means it's
            % taking too long and you should reduce the computational load.
            % the x matrix has nchs columns, each containing one audio channel
            count = 0;
        else
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