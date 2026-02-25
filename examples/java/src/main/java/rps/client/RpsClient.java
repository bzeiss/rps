package rps.client;

import io.grpc.ManagedChannel;
import io.grpc.ManagedChannelBuilder;
import io.grpc.StatusRuntimeException;
import rps.v1.Rps.GetStatusRequest;
import rps.v1.Rps.GetStatusResponse;
import rps.v1.RpsServiceGrpc;
import rps.v1.Rps.ScanEvent;
import rps.v1.Rps.StartScanRequest;
import rps.v1.Rps.StopScanRequest;
import rps.v1.Rps.StopScanResponse;
import rps.v1.Rps.ShutdownRequest;

import java.util.Iterator;
import java.util.List;
import java.util.concurrent.TimeUnit;

public class RpsClient implements AutoCloseable {
    private final ManagedChannel channel;
    private final RpsServiceGrpc.RpsServiceBlockingStub blockingStub;

    public RpsClient(String host, int port) {
        this(ManagedChannelBuilder.forAddress(host, port)
                .usePlaintext()
                .build());
    }

    private RpsClient(ManagedChannel channel) {
        this.channel = channel;
        this.blockingStub = RpsServiceGrpc.newBlockingStub(channel);
    }

    public Iterator<ScanEvent> startScan(List<String> scanDirs, String singlePlugin, 
                                         String mode, String formats, String filter, 
                                         int limit, int jobs, int retries, 
                                         int timeoutMs, boolean verbose) {
        StartScanRequest request = StartScanRequest.newBuilder()
                .addAllScanDirs(scanDirs)
                .setSinglePlugin(singlePlugin)
                .setMode(mode)
                .setFormats(formats)
                .setFilter(filter)
                .setLimit(limit)
                .setJobs(jobs)
                .setRetries(retries)
                .setTimeoutMs(timeoutMs)
                .setVerbose(verbose)
                .build();
        // Streaming calls don't typically use short deadlines for the whole stream, 
        // but we'll let the user manage it.
        return blockingStub.startScan(request);
    }

    public GetStatusResponse getStatus() {
        return blockingStub.withDeadlineAfter(5, TimeUnit.SECONDS)
                .getStatus(GetStatusRequest.getDefaultInstance());
    }

    public StopScanResponse stopScan() {
        return blockingStub.withDeadlineAfter(5, TimeUnit.SECONDS)
                .stopScan(StopScanRequest.getDefaultInstance());
    }

    public void shutdown() {
        try {
            blockingStub.withDeadlineAfter(3, TimeUnit.SECONDS)
                    .shutdown(ShutdownRequest.getDefaultInstance());
        } catch (StatusRuntimeException e) {
            // Server may close connection before responding
        }
    }

    @Override
    public void close() throws Exception {
        channel.shutdown().awaitTermination(5, TimeUnit.SECONDS);
    }
}
