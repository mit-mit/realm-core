#!groovy

stage('Checking') {
    parallelExecutors = [
        checkiOSSimulator_Debug : doBuildApplePlatform('iphonesimulator', 'Debug', true),
        buildAppleTV_Debug      : doBuildApplePlatform('appletvos', 'Debug', false),
    ]
    parallel parallelExecutors
}

def doBuildApplePlatform(String platform, String buildType, boolean test = false) {
    echo "env: ${env} ${env.getEnvironment()}"
    return {
        node('osx') {
            echo "Running on ${env.NODE_NAME} in ${env.WORKSPACE}"
            sh 'env'
        }
    }
}
