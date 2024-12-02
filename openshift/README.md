# Using `b1b` on OpenShift

`b1b` can be run as a `DaemonSet` on Red Hat OpenShift. (These instructions
should be adaptable to other Kubernetes distributions as well.)

> **NOTE**
>
> These instructions assume that a `b1b` container image has been built and
> pushed to a container image registry, following the instructions
> [here](../oci/README.md).

1. Create the `b1b` namespace.

   ```
   $ oc create namespace b1b
   namespace/b1b created
   ```

1. Create a service account.

   ```
   $ oc create serviceaccount b1b -n b1b
   serviceaccount/b1b created
   ```

1. Add the service account to the `privileged` SCC
   (`SecurityContextConstraint`).

   ```
   $ oc adm policy add-scc-to-user privileged system:serviceaccount:b1b:b1b
   clusterrole.rbac.authorization.k8s.io/system:openshift:scc:privileged added: "b1b"
   ```

1. Edit `ds-b1b.yaml` and set `.spec.template.spec.containers[0].image` to the
   URL of the `b1b` container image.

   > **NOTE**
   >
   > `b1b` will exit with an error status on any node that does not have a
   > suitable network interface (a mode 1 bond that is attached to a Linux or
   > Open vSwitch bridge).  If necessary, a node selector should be used to
   > ensure that the `DaemonSet` only creates `b1b` pods on nodes that have
   > one or more suitable interfaces.

1. Create the `DaemonSet`.

   ```
   $ oc create -f ds-b1b.yaml
   daemonset.apps/b1b created
   ```

1. Check that the expected number of pods are running.

   ```
   $ oc get pods -n b1b
   NAME        READY   STATUS    RESTARTS   AGE
   b1b-7x8vf   1/1     Running   0          41s
   b1b-c6lcg   1/1     Running   0          41s
   b1b-fqcnj   1/1     Running   0          41s
   b1b-gpcp8   1/1     Running   0          41s
   b1b-gpknf   1/1     Running   0          41s
   b1b-nqvjg   1/1     Running   0          41s
   ```
