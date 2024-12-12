import java.util.Scanner;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.ReentrantLock;
import java.util.Iterator;
import java.util.ListIterator;
import java.util.LinkedList;

class Graph {
    int sourceIndex;       // Index of the source node
    int sinkIndex;         // Index of the sink node
    int nodeCount;         // Total number of nodes
    int edgeCount;         // Total number of edges

    int activeThreadCount; // Number of threads currently doing work
    Node excessListHead;   // Head of the linked list of nodes that currently have excess flow (preflow)
    Node[] nodes;          // Array of nodes
    Edge[] edges;          // Array of edges

    ReentrantLock globalLock; 
    Condition conditionForWork;

    Graph(Node[] nodes, Edge[] edges) {
        this.nodes = nodes;
        this.nodeCount = nodes.length;
        this.edges = edges;
        this.edgeCount = edges.length;

        // Lock and condition to synchronize threads that are pushing flow
        globalLock = new ReentrantLock();
        conditionForWork = globalLock.newCondition();

        activeThreadCount = 0;
    }

    // Add a node to the excess list if it is not the source or sink
    void addExcessNode(Node node) {
        if (node != nodes[sourceIndex] && node != nodes[sinkIndex]) {
            globalLock.lock();
            node.next = excessListHead;
            excessListHead = node;
            // Signal one waiting thread that there is work to do
        
            globalLock.unlock();
        }
    }

    // Given an edge and a node, return the opposite endpoint of that edge
    Node getOtherNode(Edge edge, Node node) {
        if (edge.u == node)
            return edge.v;
        else
            return edge.u;
    }

    // Increase the height (label) of a node by one and put it back into the excess list
    void relabelNode(Node u) {
        u.nodeLock.lock();
        u.height += 1;
        u.nodeLock.unlock();

        addExcessNode(u);
    }

    // Push flow from node u to node v through edge e
    void pushFlow(Node u, Node v, Edge e) {
        int flowPushAmount;

        // Determine how much flow can be pushed
        if (u == e.u) {
            // Forward edge
            flowPushAmount = Math.min(u.excessFlow, e.capacity - e.flow);
            e.flow += flowPushAmount;
        } else {
            // Reverse edge
            flowPushAmount = Math.min(u.excessFlow, e.capacity + e.flow);
            e.flow -= flowPushAmount;
        }

        // Adjust excess flows on both nodes
        u.excessFlow -= flowPushAmount;
        v.excessFlow += flowPushAmount;

        // Assertions to ensure correctness
        assert(flowPushAmount >= 0);
        assert(u.excessFlow >= 0);
        assert(Math.abs(e.flow) <= e.capacity);

        // If u still has excess flow, add it back to the excess list
        if (u.excessFlow > 0) {
            addExcessNode(u);
        }

        // If v just gained excess flow equal to flowPushAmount from zero, add it to the excess list
        if (v.excessFlow == flowPushAmount) {
            addExcessNode(v);
        }
    }

    // Try to push flow from node u. If no push is possible, relabel the node
    void dispatch(Node u) {
        Iterator<Edge> edgeIterator = u.adjacencyList.listIterator();
        Node v = null;
        Edge currentEdge = null;
        int directionFlag = 0;

        // Look for an admissible edge (where pushing is possible)
        while (edgeIterator.hasNext()) {
            currentEdge = edgeIterator.next();

            // Determine direction
            if (u == currentEdge.u) {
                v = currentEdge.v;
                directionFlag = 1;
            } else {
                v = currentEdge.u;
                directionFlag = -1;
            }

            // Lock the nodes in order based on their indices to avoid deadlock
            if (u.index < v.index) {
                u.nodeLock.lock();
                v.nodeLock.lock();
            } else {
                v.nodeLock.lock();
                u.nodeLock.lock();
            }

            // Check if the "height" condition for pushing is satisfied and capacity is available
            if (u.height > v.height && directionFlag * currentEdge.flow < currentEdge.capacity) {
                // Found an edge through which we can push flow
                break;
            } else {
                // Release locks if this edge doesn't work
                u.nodeLock.unlock();
                v.nodeLock.unlock();
                v = null;
            }
        }

        // If we found a valid edge (v is not null), push flow. Otherwise, relabel u.
        if (v != null) {
            pushFlow(u, v, currentEdge);
            u.nodeLock.unlock();
            v.nodeLock.unlock();
        } else {
            relabelNode(u);
        }
    }

    // Thread function to continuously process excess nodes
    void pushThreadFunction() throws InterruptedException {
        Node u;
        while (true) {
            globalLock.lock();
            u = excessListHead;
            excessListHead = (excessListHead == null ? null : excessListHead.next);

            // If no excess node is available, wait for one unless all threads are idle and done
            while (u == null) {
                if (activeThreadCount == 0) {
                    globalLock.unlock();
                    return; // No more work, exit the thread
                }

                conditionForWork.await();
                u = excessListHead;
                excessListHead = (excessListHead == null ? null : excessListHead.next);
            }

            // Mark that this thread is now active
            activeThreadCount += 1;
            globalLock.unlock();

            // Process the node u
            dispatch(u);

            // After done processing, mark the thread as inactive and signal any waiting threads
            globalLock.lock();
            activeThreadCount -= 1;
            conditionForWork.signalAll();
            globalLock.unlock();
        }
    }

    // Main preflow function that initializes the flow and runs the push threads
    int preflow(int s, int t) {
        this.sourceIndex = s;
        this.sinkIndex = t;

        // Initialize preflow from the source
        nodes[s].height = nodeCount;

        ListIterator<Edge> edgeIter = nodes[s].adjacencyList.listIterator();
        while (edgeIter.hasNext()) {
            Edge e = edgeIter.next();
            nodes[s].excessFlow += e.capacity;
            pushFlow(nodes[s], getOtherNode(e, nodes[s]), e);
        }

        activeThreadCount = 0;
        Thread[] workerThreads = new Thread[10]; // Using 10 worker threads

        for (int i = 0; i < 10; i++) {
            workerThreads[i] = new Thread(new Runnable() {
                public void run() {
                    try {
                        pushThreadFunction();
                    } catch (InterruptedException e) {
                        e.printStackTrace();
                    }
                }
            });

            workerThreads[i].start();
        }

        // Wait for all threads to complete
        for (Thread wt : workerThreads) {
            try {
                wt.join();
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
        }

        // The excess flow at the sink node is the maximum flow
        return nodes[t].excessFlow;
    }
}

class Node {
    int height;       // Label (height) of the node
    int excessFlow;   // Excess flow currently at this node
    int index;        // Node index (used for ordering locks)
    Node next;        // Next node in the linked list of excess nodes

    LinkedList<Edge> adjacencyList; // List of edges connected to this node
    ReentrantLock nodeLock;         // Per-node lock

    Node(int index) {
        this.index = index;
        adjacencyList = new LinkedList<Edge>();
        nodeLock = new ReentrantLock();
    }
}

class Edge {
    Node u;          // One endpoint of the edge
    Node v;          // The other endpoint of the edge
    int flow;        // Current flow on the edge
    int capacity;    // Capacity of the edge

    Edge(Node u, Node v, int capacity) {
        this.u = u;
        this.v = v;
        this.capacity = capacity;
        // flow defaults to 0
    }
}

class Preflow {
    public static void main(String args[]) {
        double begin = System.currentTimeMillis();
        Scanner input = new Scanner(System.in);

        int n = input.nextInt(); // number of nodes
        int m = input.nextInt(); // number of edges
        input.nextInt(); // Skip (these might represent unused parameters)
        input.nextInt(); // Skip

        Node[] nodeArray = new Node[n];
        Edge[] edgeArray = new Edge[m];

        // Initialize nodes
        for (int i = 0; i < n; i++) {
            nodeArray[i] = new Node(i);
        }

        // Read edges and build the graph
        for (int i = 0; i < m; i++) {
            int u = input.nextInt();
            int v = input.nextInt();
            int c = input.nextInt();

            edgeArray[i] = new Edge(nodeArray[u], nodeArray[v], c);
            nodeArray[u].adjacencyList.addLast(edgeArray[i]);
            nodeArray[v].adjacencyList.addLast(edgeArray[i]);
        }

        Graph g = new Graph(nodeArray, edgeArray);
        int maxFlow = g.preflow(0, n - 1);
        double end = System.currentTimeMillis();

        System.out.println("t = " + (end - begin) / 1000.0 + " s");
        System.out.println("f = " + maxFlow);
    }
}
